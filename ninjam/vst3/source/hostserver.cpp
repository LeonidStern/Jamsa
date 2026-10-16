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
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "ninjamsrv_embedded.h"

namespace ninjam_vst3 {

static const size_t MAX_MESSAGES = 200;

// Anyone can join without an account, any player can change BPM and BPI (a single
// vote passes) or switch the room to Opus or FLAC (!codec), and the server asks the
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
    "PortMapping yes\n";

HostServer &HostServer::instance()
{
  static HostServer s;
  return s;
}

std::string HostServer::writeConfig(int port)
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
  fprintf(fp, CONFIG, port);
  const bool ok = !ferror(fp);
  fclose(fp);
  return ok ? path : std::string();
}

bool HostServer::start(const void *owner, std::string *error)
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

  std::string err;
  for (int i = 0; i < PORTS_TO_TRY; i ++)
  {
    const int port = FIRST_PORT + i;
    const std::string cfg = writeConfig(port);
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
  std::lock_guard<std::mutex> lock(m_mutex);
  m_state = State();
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
