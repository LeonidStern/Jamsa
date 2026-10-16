/*
    NINJAM VST3 - hostserver.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "hostserver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>

#include "ninjamsrv_embedded.h"

namespace ninjam_vst3 {

static const size_t MAX_MESSAGES = 200;

// Anyone can join without an account, any player can change BPM and BPI (a single
// vote passes), switch the room to Opus or FLAC (!codec) or change its Opus bitrate
// (!bitrate, at first the host's own), and the server asks the
// router to forward its port. Two players with one name from one address (two
// instances in a DAW that both keep the default name) both stay: the second is name.2.
static const char *CONFIG =
    "# Written by Jamsa each time it starts its server.\n"
    "Port %d\n"
    "MaxUsers 8\n"
    "MaxChannels 32 32\n"
    "AnonymousUsers multi\n"
    "AnonymousUsersCanChat yes\n"
    "AnonymousMaskIP no\n"
    "DefaultBPM 120\n"
    "DefaultBPI 16\n"
    "DefaultTopic \"Hosted by Jamsa. Type !vote bpm 100, !vote bpi 8 or !codec flac to change the room\"\n"
    "SetVotingThreshold 1\n"
    "SetVotingVoteTimeout 60\n"
    "AllowCodecs opus flac\n"
    "RequireCodec opus\n" // older clients (Vorbis only) are refused, so the room never falls back to Vorbis
    "OpusBitrate %d\n"
    "PortMapping yes\n";

HostServer &HostServer::instance()
{
  static HostServer s;
  return s;
}

std::string HostServer::writeConfig(int port, int opusKbps)
{
  std::string dir;
#ifdef _WIN32
  char tmp[MAX_PATH + 1] = "";
  if (GetTempPathA(sizeof(tmp), tmp)) dir = tmp;
  const int pid = _getpid();
#else
  const char *t = getenv("TMPDIR");
  dir = t && *t ? t : "/tmp";
  const int pid = (int)getpid();
#endif
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') dir += "/";
  char name[64];
  snprintf(name, sizeof(name), "ninjam-plugin-server-%d.cfg", pid);
  const std::string path = dir + name;

  FILE *fp = fopen(path.c_str(), "w");
  if (!fp) return std::string();
  fprintf(fp, CONFIG, port, opusKbps);
  const bool ok = !ferror(fp);
  fclose(fp);
  return ok ? path : std::string();
}

bool HostServer::start(const void *owner, int opusKbps, std::string *error)
{
  std::lock_guard<std::mutex> busy(m_startStop);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_state.owner == owner) return true;
    if (m_state.owner)
    {
      if (error)
      {
        char buf[200];
        snprintf(buf, sizeof(buf), "Another Jamsa in this program already hosts a server (port %d). Connect to localhost:%d to join it.",
                 m_state.port, m_state.port);
        *error = buf;
      }
      return false;
    }
  }

  {
    // messages are for the server about to start
    std::lock_guard<std::mutex> lock(m_mutex);
    m_messages.clear();
  }
  stopLookup(); // (stop() has joined the last one already)

  std::string err;
  for (int i = 0; i < PORTS_TO_TRY; i ++)
  {
    const int port = FIRST_PORT + i;
    const std::string cfg = writeConfig(port, opusKbps);
    if (cfg.empty())
    {
      err = "Could not write the server's settings to the temporary folder.";
      break;
    }
    const int r = ninjamsrv_embedded_start(cfg.c_str(), onLog, this);
    remove(cfg.c_str()); // the server reads its settings once, when it starts
    if (r == NINJAMSRV_OK)
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_state = State();
      m_state.owner = owner;
      m_state.port = port;
      m_state.portmapPending = true;
      const std::string local = localNetworkAddress();
      if (!local.empty()) m_state.localAddress = local + ":" + std::to_string(port);
      m_state.lookupPending = true;
      m_lookupStop = false;
      m_lookup = std::thread([this, port]() { lookUpPublicAddress(port); });
      return true;
    }
    if (r == NINJAMSRV_LISTEN_ERROR)
    {
      char buf[128];
      snprintf(buf, sizeof(buf), "Ports %d to %d are in use by other programs.", FIRST_PORT, port);
      err = buf;
      continue;
    }
    err = r == NINJAMSRV_ALREADY_RUNNING ? "A NINJAM server is already running in this program." : "The server could not read its settings.";
    break;
  }
  if (error) *error = err;
  return false;
}

void HostServer::stop(const void *owner)
{
  std::lock_guard<std::mutex> busy(m_startStop);
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_state.owner || m_state.owner != owner) return;
  }
  ninjamsrv_embedded_stop(); // also removes the router's port forward; no more log calls after this
  stopLookup();
  std::lock_guard<std::mutex> lock(m_mutex);
  m_state = State();
}

HostServer::~HostServer()
{
  stopLookup(); // the module is being unloaded: its code must not run on in that thread
}

void HostServer::stopLookup()
{
  m_lookupStop = true;
  if (m_lookup.joinable()) m_lookup.join();
}

HostServer::State HostServer::state()
{
  State st;
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    st = m_state;
  }
  if (st.owner) st.users = ninjamsrv_embedded_users(nullptr, 0);
  return st;
}

std::vector<std::string> HostServer::messagesSince(unsigned long long *serial)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<std::string> out;
  const unsigned long long first = m_serial - m_messages.size();
  if (*serial < first) *serial = first;
  for (unsigned long long i = *serial; i < m_serial; i ++) out.push_back(m_messages[(size_t)(i - first)]);
  *serial = m_serial;
  return out;
}

static bool starts_with(const std::string &s, const char *prefix)
{
  return s.compare(0, strlen(prefix), prefix) == 0;
}

static bool starts_with_nocase(const std::string &s, const char *prefix)
{
  const size_t n = strlen(prefix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i ++)
  {
    char a = s[i], b = prefix[i];
    if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
    if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
    if (a != b) return false;
  }
  return true;
}

void HostServer::parseLogLine(const std::string &line, State &st, bool *show)
{
  *show = false;
  if (starts_with(line, "[portmap]"))
  {
    std::string text = line.substr(9);
    while (!text.empty() && text[0] == ' ') text.erase(0, 1);
    *show = true;

    static const char *reachable = "people can connect to ";
    const size_t at = line.find(reachable);
    if (at != std::string::npos)
    {
      std::string addr = line.substr(at + strlen(reachable));
      while (!addr.empty() && (addr.back() == ' ' || addr.back() == '.')) addr.pop_back();
      st.publicAddress = addr;
      st.portmapNote.clear();
      st.portmapPending = false;
    }
    else if (line.find("asking the router") != std::string::npos)
    {
      st.portmapPending = true;
    }
    else if (line.find("removed the forward") != std::string::npos)
    {
      st.publicAddress.clear();
      st.portmapPending = false;
    }
    else if (line.find("lost the forward") != std::string::npos)
    {
      st.publicAddress.clear();
      st.portmapNote = text;
      st.portmapPending = true;
    }
    else
    {
      st.publicAddress.clear();
      st.portmapNote = text;
      st.portmapPending = false;
    }
  }
  else if (starts_with(line, "Error listening"))
  {
    // a port that was taken while starting; start() tries the next one
  }
  else if (starts_with_nocase(line, "error") || starts_with_nocase(line, "warning"))
  {
    *show = true;
  }
}

void HostServer::onLog(void *ctx, const char *text)
{
  HostServer *_this = (HostServer *)ctx;
  std::string line(text ? text : "");
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
  if (line.empty()) return;

  std::lock_guard<std::mutex> lock(_this->m_mutex);
  bool show = false;
  parseLogLine(line, _this->m_state, &show);
  if (show)
  {
    _this->m_messages.push_back(line);
    if (_this->m_messages.size() > MAX_MESSAGES) _this->m_messages.erase(_this->m_messages.begin());
    _this->m_serial ++;
  }
}

// ---------------------------------------------------------------------------
// The public address, from a "what's my IP" service
// ---------------------------------------------------------------------------

bool HostServer::parseWhatsMyIpReply(const std::string &reply, std::string *ip)
{
  if (reply.compare(0, 5, "HTTP/") != 0) return false;
  const size_t code = reply.find(' ');
  if (code == std::string::npos || reply.compare(code + 1, 4, "200 ") != 0) return false;
  const size_t body = reply.find("\r\n\r\n");
  if (body == std::string::npos) return false;
  std::string t = reply.substr(body + 4);
  while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
  // four numbers from 0 to 255 and nothing else
  int parts = 0, value = -1;
  for (char c : t)
  {
    if (c >= '0' && c <= '9')
    {
      value = (value < 0 ? 0 : value * 10) + (c - '0');
      if (value > 255) return false;
    }
    else if (c == '.' && value >= 0 && parts < 3)
    {
      parts ++;
      value = -1;
    }
    else return false;
  }
  if (parts != 3 || value < 0) return false;
  *ip = t;
  return true;
}

#ifdef _WIN32
typedef SOCKET socket_t;
static const socket_t NO_SOCKET = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
static bool connect_in_progress() { return WSAGetLastError() == WSAEWOULDBLOCK; }
#else
typedef int socket_t;
static const socket_t NO_SOCKET = -1;
static void close_socket(socket_t s) { close(s); }
static bool connect_in_progress() { return errno == EINPROGRESS; }
#endif

// waits until s can be written to (or read from), at most until deadline or until *stop
static bool wait_socket(socket_t s, bool write, std::chrono::steady_clock::time_point deadline, const std::atomic<bool> *stop)
{
  for (;;)
  {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (left <= 0 || *stop) return false;
    const int slice = (int)std::min<long long>(left, 100); // so stop() never waits long
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = slice * 1000;
    const int r = select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
#else
    // poll, not select: a host can have more files open than an fd_set holds
    pollfd p;
    p.fd = s;
    p.events = write ? POLLOUT : POLLIN;
    p.revents = 0;
    const int r = poll(&p, 1, slice);
#endif
    if (r != 0) return r > 0;
  }
}

// A plain HTTP GET (the services answer in a few hundred bytes): the reply, or "" within
// timeoutMs. Non-blocking, so a stalled service holds nothing up for longer.
static std::string http_get(const std::string &host, int port, const std::string &path, int timeoutMs, const std::atomic<bool> *stop)
{
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  addrinfo hints, *res = nullptr;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return "";
  std::string reply;
  const socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s != NO_SOCKET)
  {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
#ifdef SO_NOSIGPIPE
    int one = 1; // macOS: a write to a closed connection mustn't kill the host
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#ifdef MSG_NOSIGNAL
    const int sendFlags = MSG_NOSIGNAL; // Linux: the same
#else
    const int sendFlags = 0;
#endif
    bool ok = connect(s, res->ai_addr, (int)res->ai_addrlen) == 0 || (connect_in_progress() && wait_socket(s, true, deadline, stop));
    if (ok)
    {
      int err = 0;
      socklen_t len = sizeof(err);
      ok = getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len) == 0 && err == 0;
    }
    const std::string request = "GET " + path + " HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: Jamsa\r\nAccept: text/plain\r\nConnection: close\r\n\r\n";
    size_t sent = 0;
    while (ok && sent < request.size())
    {
      if (!wait_socket(s, true, deadline, stop)) ok = false;
      else
      {
        const int n = (int)send(s, request.data() + sent, (int)(request.size() - sent), sendFlags);
        if (n <= 0) ok = false;
        else sent += (size_t)n;
      }
    }
    while (ok && reply.size() < 8192 && wait_socket(s, false, deadline, stop))
    {
      char buf[1024];
      const int n = (int)recv(s, buf, sizeof(buf), 0);
      if (n <= 0) break; // closed: the whole reply is in
      reply.append(buf, (size_t)n);
    }
    close_socket(s);
  }
  freeaddrinfo(res);
  return reply;
}

void HostServer::lookUpPublicAddress(int port)
{
#ifdef _WIN32
  WSADATA wsa;
  const bool wsaOk = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif
  struct Service { std::string host; int port; std::string path; };
  std::vector<Service> services;
  const char *env = getenv("JAMSA_WHATSMYIP");
  if (env && *env)
  {
    // "host:port/path", for the tests; "off" asks nobody
    const std::string e = env;
    if (e != "off")
    {
      const size_t slash = e.find('/'), colon = e.find(':');
      Service sv;
      sv.host = e.substr(0, (std::min)(colon, slash));
      sv.port = colon < slash ? atoi(e.substr(colon + 1, slash - colon - 1).c_str()) : 80;
      sv.path = slash != std::string::npos ? e.substr(slash) : "/";
      services.push_back(sv);
    }
  }
  else services = { { "api.ipify.org", 80, "/" }, { "checkip.amazonaws.com", 80, "/" } };

  std::string ip;
  for (const Service &sv : services)
  {
    if (m_lookupStop) break;
    if (parseWhatsMyIpReply(http_get(sv.host, sv.port, sv.path, 4000, &m_lookupStop), &ip)) break;
    ip.clear();
  }
#ifdef _WIN32
  if (wsaOk) WSACleanup();
#endif
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state.port != port) return; // stopped meanwhile
  if (!ip.empty()) m_state.lookedUpAddress = ip + ":" + std::to_string(port);
  m_state.lookupPending = false;
}

std::string localNetworkAddress()
{
  // the address a packet to the internet would leave from; connecting a UDP socket sends nothing
  std::string out;
#ifdef _WIN32
  SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return out;
#else
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return out;
#endif
  sockaddr_in to;
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  to.sin_addr.s_addr = htonl(0x08080808); // 8.8.8.8
  if (connect(s, (sockaddr *)&to, sizeof(to)) == 0)
  {
    sockaddr_in me;
    socklen_t len = sizeof(me);
    if (getsockname(s, (sockaddr *)&me, &len) == 0 && me.sin_addr.s_addr != htonl(0))
    {
      const unsigned int a = ntohl(me.sin_addr.s_addr);
      char buf[32];
      snprintf(buf, sizeof(buf), "%u.%u.%u.%u", a >> 24, (a >> 16) & 255, (a >> 8) & 255, a & 255);
      out = buf;
    }
  }
#ifdef _WIN32
  closesocket(s);
#else
  close(s);
#endif
  return out;
}

} // namespace ninjam_vst3
