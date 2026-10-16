/*
    NINJAM Server - liverelay.cpp

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Live mode's UDP relay, see liverelay.h.

  It logs a summary of its traffic at most once a minute, while there is any.
  NINJAM_LIVE_STATS_SECONDS in the environment changes how often, for the tests.

*/

#include "../../WDL/jnetlib/netinc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
// RtlGenRandom, from advapi32
extern "C" BOOLEAN NTAPI SystemFunction036(PVOID buffer, ULONG length);
#ifdef _MSC_VER
#pragma comment(lib,"advapi32.lib")
#endif
#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#endif
#else
#include <poll.h>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define LIVE_HAVE_ARC4RANDOM // arc4random_buf, in stdlib.h
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

#include "liverelay.h"

void logText(const char *s, ...);

LiveRelay g_live;

namespace {

// the datagrams, see ../mpb.h
enum
{
  UDP_VERSION = 1,
  TYPE_HELLO = 1,         // HELLO, and the server's HELLO_ACK
  TYPE_DATA = 2,
  CLIENT_HEADER = 20,
  SERVER_HEADER = 8,
  HELLO_SIZE = 32,
  HELLO_ACK_SIZE = 16,
  MAX_PAYLOAD = 1452,
  MAX_DATAGRAM = CLIENT_HEADER + MAX_PAYLOAD,
  MAX_DESTS = 64,          // more than a room can have (LiveMaxPeers is at most 64)
};

const double FRESH_SECONDS = 20.0; // a player not heard from for this long gets no more audio
const double BURST_SECONDS = 0.25;
const int SOCKET_BUFFER = 1 << 20;

// why datagrams were dropped, for the summary in the log
enum
{
  DROP_MALFORMED, DROP_TOO_BIG, DROP_UNKNOWN_SENDER, DROP_WRONG_KEY, DROP_WRONG_ADDRESS,
  DROP_RATE, DROP_ROOM_OFF, DROP_MAY_NOT_SEND, DROP_NO_SUCH_PLAYER, DROP_SEND_FAILED,
  NUM_DROPS
};
const char * const drop_names[NUM_DROPS] =
{
  "malformed", "too big", "unknown sender", "wrong key", "wrong address",
  "over the rate limit", "room's Live is off", "sender may not send", "no such player", "could not send",
};

struct Peer
{
  unsigned int room;
  unsigned int tcp_ip;
  WDL_UINT64 key;
  bool may_send;
  bool bound; // addr is where it last sent from
  bool fresh; // heard from in the last FRESH_SECONDS; PeerBound() and Generation() follow it
  struct sockaddr_in addr;
  double last_rx;
  double tokens_bytes, tokens_packets, tokens_time; // rate limit; tokens_time < 0 until the first datagram
};

struct Room
{
  Room() : on(false), kbps(128), frame(240) { }
  bool on;
  int kbps, frame;
  std::vector<unsigned int> peers;
};

double now_seconds()
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

unsigned int get_le32(const unsigned char *p)
{
  return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

WDL_UINT64 get_le64(const unsigned char *p)
{
  return (WDL_UINT64)get_le32(p) | ((WDL_UINT64)get_le32(p + 4) << 32);
}

void put_le32(unsigned char *p, unsigned int v)
{
  p[0] = v & 0xff;
  p[1] = (v >> 8) & 0xff;
  p[2] = (v >> 16) & 0xff;
  p[3] = (v >> 24) & 0xff;
}

// takes as long whichever bits differ
bool same_key(WDL_UINT64 a, WDL_UINT64 b)
{
  WDL_UINT64 d = a ^ b;
  d |= d >> 32;
  d |= d >> 16;
  d |= d >> 8;
  return (d & 0xff) == 0;
}

bool random_bytes(void *buf, size_t len)
{
#ifdef _WIN32
  return SystemFunction036(buf, (ULONG)len) != FALSE;
#elif defined(LIVE_HAVE_ARC4RANDOM)
  arc4random_buf(buf, len);
  return true;
#else
  unsigned char *p = (unsigned char *)buf;
#ifdef SYS_getrandom
  while (len > 0)
  {
    const long r = syscall(SYS_getrandom, p, len, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break; // e.g. a kernel without getrandom: read the device instead
    p += r;
    len -= (size_t)r;
  }
  if (!len) return true;
#endif
  FILE *fp = fopen("/dev/urandom", "rb");
  if (!fp) return false;
  const size_t got = fread(p, 1, len, fp);
  fclose(fp);
  return got == len;
#endif
}

} // namespace

struct LiveRelay::State
{
  State() : sock(INVALID_SOCKET), port(0), stop(false), strict(true), generation(0) { }

  SOCKET sock;
  int port;
  std::thread thread;
  std::atomic<bool> stop;

  std::mutex mutex; // guards what follows
  std::map<unsigned int, Peer> peers;
  std::map<unsigned int, Room> rooms;
  bool strict;

  std::atomic<unsigned int> generation;

  // the relay thread's own
  double stats_start, stats_seconds;
  unsigned long long received, sent, dropped[NUM_DROPS];

  void Run();
  void Handle(unsigned char *buf, int len, const struct sockaddr_in &from, double now);
  void Housekeeping(double now);
  void SendTo(const unsigned char *buf, int len, const struct sockaddr_in &to);
  bool TakeTokens(Peer &p, const Room *room, int len, double now);

  void LeaveRoom(unsigned int id, unsigned int room_id)
  {
    std::map<unsigned int, Room>::iterator r = rooms.find(room_id);
    if (r == rooms.end()) return;
    std::vector<unsigned int> &v = r->second.peers;
    for (size_t i = 0; i < v.size(); i ++)
    {
      if (v[i] == id)
      {
        v.erase(v.begin() + i);
        break;
      }
    }
  }

  void ResetStats(double now)
  {
    stats_start = now;
    received = sent = 0;
    memset(dropped, 0, sizeof(dropped));
  }
};

LiveRelay::LiveRelay() : m_state(new State) { }

LiveRelay::~LiveRelay()
{
  Close();
  delete m_state;
  m_state = NULL;
}

bool LiveRelay::RandomKey(WDL_UINT64 *key)
{
  for (int tries = 0; tries < 4; tries ++)
  {
    unsigned char b[8];
    if (!random_bytes(b, sizeof(b))) return false;
    *key = get_le64(b);
    if (*key) return true; // 0 means "no key"
  }
  return false;
}

bool LiveRelay::Open(int port)
{
  Close();
  State *st = m_state;
  if (!st || port <= 0 || port > 65535) return false;

  SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s == INVALID_SOCKET) return false;

  // No SO_REUSEADDR: a second server (another Jamsa hosting on this machine) must get
  // an error and try another port, rather than share this one
#ifdef _WIN32
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&one, sizeof(one));
#endif
  int bufsize = SOCKET_BUFFER;
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char *)&bufsize, sizeof(bufsize));
  bufsize = SOCKET_BUFFER;
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&bufsize, sizeof(bufsize));
  SET_SOCK_BLOCK(s, 0);

  struct sockaddr_in sin;
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_port = htons((unsigned short)port);
  sin.sin_addr.s_addr = INADDR_ANY;
  if (bind(s, (struct sockaddr *)&sin, sizeof(sin)))
  {
    closesocket(s);
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(st->mutex);
    st->peers.clear();
    st->rooms.clear();
  }
  st->sock = s;
  st->port = port;
  st->stop = false;
  st->stats_seconds = 60.0;
  const char *env = getenv("NINJAM_LIVE_STATS_SECONDS");
  if (env && atof(env) > 0) st->stats_seconds = atof(env);
  st->ResetStats(now_seconds());
  st->thread = std::thread([st] { st->Run(); });
  st->generation ++;
  return true;
}

void LiveRelay::Close()
{
  State *st = m_state;
  if (!st || st->sock == INVALID_SOCKET) return;
  st->stop = true;
  st->thread.join(); // it checks stop at least every 100 ms
  closesocket(st->sock);
  st->sock = INVALID_SOCKET;
  st->port = 0;
  {
    std::lock_guard<std::mutex> lock(st->mutex);
    st->peers.clear();
    st->rooms.clear();
  }
  st->generation ++;
}

bool LiveRelay::IsOpen() const
{
  return m_state && m_state->sock != INVALID_SOCKET;
}

int LiveRelay::Port() const
{
  return m_state ? m_state->port : 0;
}

void LiveRelay::SetStrictAddress(bool strict)
{
  if (!m_state) return;
  std::lock_guard<std::mutex> lock(m_state->mutex);
  m_state->strict = strict;
}

void LiveRelay::SetPeer(unsigned int id, unsigned int room_id, unsigned int tcp_ip, WDL_UINT64 key, bool may_send)
{
  State *st = m_state;
  if (!st || !id) return;
  std::lock_guard<std::mutex> lock(st->mutex);
  std::map<unsigned int, Peer>::iterator it = st->peers.find(id);
  if (it == st->peers.end())
  {
    Peer p;
    memset(&p, 0, sizeof(p));
    p.room = room_id;
    p.tcp_ip = tcp_ip;
    p.key = key;
    p.may_send = may_send;
    p.tokens_time = -1.0;
    st->peers[id] = p;
    st->rooms[room_id].peers.push_back(id);
    return;
  }
  Peer &p = it->second;
  if (p.room != room_id)
  {
    st->LeaveRoom(id, p.room);
    st->rooms[room_id].peers.push_back(id);
    p.room = room_id;
  }
  if (p.tcp_ip != tcp_ip || p.key != key)
  {
    p.tcp_ip = tcp_ip;
    p.key = key;
    p.bound = false;
    if (p.fresh)
    {
      p.fresh = false;
      st->generation ++;
    }
  }
  p.may_send = may_send;
}

void LiveRelay::RemovePeer(unsigned int id)
{
  State *st = m_state;
  if (!st) return;
  std::lock_guard<std::mutex> lock(st->mutex);
  std::map<unsigned int, Peer>::iterator it = st->peers.find(id);
  if (it == st->peers.end()) return;
  st->LeaveRoom(id, it->second.room);
  if (it->second.fresh) st->generation ++;
  st->peers.erase(it);
}

void LiveRelay::SetRoomLive(unsigned int room_id, bool on, int live_kbps, int frame_samples)
{
  State *st = m_state;
  if (!st) return;
  std::lock_guard<std::mutex> lock(st->mutex);
  Room &r = st->rooms[room_id];
  r.on = on;
  if (live_kbps > 0) r.kbps = live_kbps;
  if (frame_samples > 0) r.frame = frame_samples;
}

void LiveRelay::RemoveRoom(unsigned int room_id)
{
  State *st = m_state;
  if (!st) return;
  std::lock_guard<std::mutex> lock(st->mutex);
  st->rooms.erase(room_id);
}

bool LiveRelay::PeerBound(unsigned int id)
{
  State *st = m_state;
  if (!st) return false;
  std::lock_guard<std::mutex> lock(st->mutex);
  std::map<unsigned int, Peer>::const_iterator it = st->peers.find(id);
  return it != st->peers.end() && it->second.bound && it->second.fresh;
}

unsigned int LiveRelay::Generation() const
{
  return m_state ? m_state->generation.load() : 0;
}

void LiveRelay::State::Run()
{
  unsigned char buf[2048]; // more than MAX_DATAGRAM, so a datagram too big shows as one
  double next_housekeeping = 0.0;
  while (!stop)
  {
    // wait up to 100 ms, which is also how soon Close() is noticed
#ifdef _WIN32
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    if (select((int)sock + 1, &fds, NULL, NULL, &tv) > 0)
#else
    // poll, because select can't wait on a descriptor numbered FD_SETSIZE or more, which
    // the socket may get when a busy server opens it on a config reload
    struct pollfd pfd;
    pfd.fd = sock;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 100) > 0)
#endif
    {
      for (int i = 0; i < 256 && !stop; i ++)
      {
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        const int n = (int)recvfrom(sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
        if (n < 0)
        {
          const int err = JNL_ERRNO;
#ifdef _WIN32
          if (err == WSAEMSGSIZE)
          {
            received ++;
            dropped[DROP_TOO_BIG] ++;
            continue;
          }
          if (err == WSAECONNRESET || err == WSAEINTR) continue; // a player that left (ICMP), a signal
#else
          if (err == EINTR) continue;
#endif
          break; // nothing more to read for now
        }
        if (fromlen < (socklen_t)sizeof(from) || from.sin_family != AF_INET) continue;
        Handle(buf, n, from, now_seconds());
      }
    }
    const double now = now_seconds();
    if (now >= next_housekeeping)
    {
      Housekeeping(now);
      next_housekeeping = now + 0.1;
    }
  }
}

bool LiveRelay::State::TakeTokens(Peer &p, const Room *room, int len, double now)
{
  const int kbps = room && room->kbps > 0 ? room->kbps : 128;
  const int frame = room && room->frame > 0 ? room->frame : 240;
  // per sender: every datagram is copied to ndest players, so what the server sends for
  // it is ndest times this
  const double bytes_per_second = kbps * 1000.0 / 8.0 * 2.0 + 8000.0;
  const double packets_per_second = 2.0 * 48000.0 / frame + 50.0;
  const double max_bytes = bytes_per_second * BURST_SECONDS;
  const double max_packets = packets_per_second * BURST_SECONDS;

  if (p.tokens_time < 0.0)
  {
    p.tokens_bytes = max_bytes;
    p.tokens_packets = max_packets;
  }
  else if (now > p.tokens_time)
  {
    const double dt = now - p.tokens_time;
    p.tokens_bytes += dt * bytes_per_second;
    if (p.tokens_bytes > max_bytes) p.tokens_bytes = max_bytes;
    p.tokens_packets += dt * packets_per_second;
    if (p.tokens_packets > max_packets) p.tokens_packets = max_packets;
  }
  p.tokens_time = now;

  if (p.tokens_bytes < len || p.tokens_packets < 1.0) return false;
  p.tokens_bytes -= len;
  p.tokens_packets -= 1.0;
  return true;
}

void LiveRelay::State::SendTo(const unsigned char *data, int len, const struct sockaddr_in &to)
{
  if (sendto(sock, (const char *)data, len, 0, (const struct sockaddr *)&to, sizeof(to)) == len) sent ++;
  else dropped[DROP_SEND_FAILED] ++; // e.g. the send buffer is full
}

void LiveRelay::State::Handle(unsigned char *buf, int len, const struct sockaddr_in &from, double now)
{
  received ++;
  if (len > MAX_DATAGRAM)
  {
    dropped[DROP_TOO_BIG] ++;
    return;
  }
  if (len < CLIENT_HEADER || buf[0] != 'N' || buf[1] != 'L' || buf[2] != UDP_VERSION ||
      (buf[3] == TYPE_HELLO ? len != HELLO_SIZE : (buf[3] != TYPE_DATA || len == CLIENT_HEADER)))
  {
    dropped[DROP_MALFORMED] ++;
    return;
  }
  const int type = buf[3];
  const unsigned int sender = get_le32(buf + 4);
  const WDL_UINT64 key = get_le64(buf + 8);
  const unsigned int dest = get_le32(buf + 16);

  struct sockaddr_in to[MAX_DESTS];
  int nto = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    std::map<unsigned int, Peer>::iterator it = peers.find(sender);
    if (it == peers.end())
    {
      dropped[DROP_UNKNOWN_SENDER] ++;
      return;
    }
    Peer &p = it->second;
    if (!same_key(p.key, key))
    {
      dropped[DROP_WRONG_KEY] ++;
      return;
    }
    if (strict && from.sin_addr.s_addr != p.tcp_ip)
    {
      dropped[DROP_WRONG_ADDRESS] ++;
      return;
    }

    // the player is where its latest good datagram came from (its router may have
    // given it another port)
    p.addr = from;
    p.bound = true;
    p.last_rx = now;
    if (!p.fresh)
    {
      p.fresh = true;
      generation ++;
    }

    std::map<unsigned int, Room>::const_iterator r = rooms.find(p.room);
    const Room *room = r != rooms.end() ? &r->second : NULL;
    if (!TakeTokens(p, room, len, now))
    {
      dropped[DROP_RATE] ++;
      return;
    }

    if (type == TYPE_DATA)
    {
      if (!room || !room->on)
      {
        dropped[DROP_ROOM_OFF] ++;
        return;
      }
      if (!p.may_send)
      {
        dropped[DROP_MAY_NOT_SEND] ++;
        return;
      }
      // only players the server knows, never an address from the datagram
      if (dest)
      {
        std::map<unsigned int, Peer>::const_iterator q = peers.find(dest);
        if (q == peers.end() || dest == sender || q->second.room != p.room ||
            !q->second.may_send || !q->second.bound || !q->second.fresh)
        {
          dropped[DROP_NO_SUCH_PLAYER] ++;
          return;
        }
        to[nto ++] = q->second.addr;
      }
      else
      {
        for (size_t i = 0; i < room->peers.size() && nto < MAX_DESTS; i ++)
        {
          if (room->peers[i] == sender) continue;
          std::map<unsigned int, Peer>::const_iterator q = peers.find(room->peers[i]);
          if (q != peers.end() && q->second.may_send && q->second.bound && q->second.fresh) to[nto ++] = q->second.addr;
        }
      }
    }
  }

  if (type == TYPE_HELLO)
  {
    unsigned char ack[HELLO_ACK_SIZE];
    ack[0] = 'N';
    ack[1] = 'L';
    ack[2] = UDP_VERSION;
    ack[3] = TYPE_HELLO;
    put_le32(ack + 4, 0);
    memcpy(ack + 8, buf + CLIENT_HEADER, 8); // the client's timestamp
    SendTo(ack, sizeof(ack), from);
    return;
  }

  // the server's header goes right before the audio, which stays where it is
  unsigned char *out = buf + CLIENT_HEADER - SERVER_HEADER;
  out[0] = 'N';
  out[1] = 'L';
  out[2] = UDP_VERSION;
  out[3] = TYPE_DATA;
  put_le32(out + 4, sender);
  for (int i = 0; i < nto; i ++) SendTo(out, len - (CLIENT_HEADER - SERVER_HEADER), to[i]);
}

void LiveRelay::State::Housekeeping(double now)
{
  int players = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (std::map<unsigned int, Peer>::iterator it = peers.begin(); it != peers.end(); ++ it)
    {
      Peer &p = it->second;
      if (p.fresh && now - p.last_rx >= FRESH_SECONDS)
      {
        p.fresh = false;
        generation ++;
      }
      if (p.fresh) players ++;
    }
  }

  const double elapsed = now - stats_start;
  if (elapsed < stats_seconds) return;
  if (received)
  {
    char buf[1024];
    unsigned long long total = 0;
    for (int i = 0; i < NUM_DROPS; i ++) total += dropped[i];
    int len = snprintf(buf, sizeof(buf), "[live] in the last %.0f s: %d player%s, %.0f datagrams/s in, %.0f out",
                       elapsed, players, players == 1 ? "" : "s", received / elapsed, sent / elapsed);
    if (!total)
    {
      len += snprintf(buf + len, sizeof(buf) - len, ", nothing dropped");
    }
    else
    {
      len += snprintf(buf + len, sizeof(buf) - len, ", dropped %llu:", total);
      bool first = true;
      for (int i = 0; i < NUM_DROPS && len < (int)sizeof(buf); i ++)
      {
        if (!dropped[i]) continue;
        len += snprintf(buf + len, sizeof(buf) - len, "%s %llu %s", first ? "" : ",", dropped[i], drop_names[i]);
        first = false;
      }
    }
    logText("%s\n", buf);
  }
  ResetStats(now);
}
