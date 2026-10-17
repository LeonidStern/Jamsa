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
  NINJAM_LIVE_STATS_SECONDS in the environment changes how often, and
  NINJAM_LIVE_FRESH_SECONDS how soon a player who stops saying HELLO gets no more
  audio (20 seconds), for the tests.

*/

#ifdef _WIN32
// WSARecvMsg and WSASendMsg (see LIVE_PKTINFO below), before netinc.h, whose
// windows.h would otherwise bring in the old winsock.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#endif

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
#include <sys/uio.h>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define LIVE_HAVE_ARC4RANDOM // arc4random_buf, in stdlib.h
#elif defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

// The relay answers each player from the address of this machine that the player sends
// to. On a machine with more than one address (an extra IP on a VPS, wired and Wi-Fi on
// one network), the routing table would otherwise pick the address replies come from,
// and the player's socket (connect()ed to the server) or its router would drop every
// reply from another one. IP_PKTINFO says which address a datagram came in on, and
// picks the one a datagram goes out from; BSD has IP_RECVDSTADDR and IP_SENDSRCADDR.
// Where neither works, the routing table picks, as for any UDP socket.
#if defined(_WIN32)
#if !defined(_WIN32_WINNT) || _WIN32_WINNT >= 0x0600 // WSASendMsg: Vista and later
#define LIVE_PKTINFO_WINSOCK
#endif
#elif (defined(__linux__) || defined(__APPLE__)) && defined(IP_PKTINFO)
#define LIVE_PKTINFO
#elif defined(IP_RECVDSTADDR) && defined(IP_SENDSRCADDR)
#define LIVE_RECVDSTADDR
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
  HELLO_SIZE = 40,
  HELLO_COOKIE = 28,       // where a HELLO echoes its cookie
  HELLO_ACK_SIZE = 24,
  MAX_PAYLOAD = 1452,
  MAX_DATAGRAM = CLIENT_HEADER + MAX_PAYLOAD,
  MAX_DESTS = 64,          // more than a room can have (LiveMaxPeers is at most 64)
};

const double FRESH_SECONDS = 20.0; // a player not heard from for this long gets no more audio
const double COOKIE_SECONDS = 30.0; // a cookie works for this long, then for as long again
const double BURST_SECONDS = 0.25;
const double DATAGRAM_OVERHEAD = 64.0; // bytes of headers in a datagram of audio, see TakeTokens
const int SOCKET_BUFFER = 1 << 20;

// why datagrams were dropped, for the summary in the log
enum
{
  DROP_MALFORMED, DROP_TOO_BIG, DROP_UNKNOWN_SENDER, DROP_WRONG_KEY, DROP_WRONG_ADDRESS,
  DROP_RATE, DROP_UNVERIFIED, DROP_ROOM_OFF, DROP_MAY_NOT_SEND, DROP_NO_SUCH_PLAYER, DROP_SEND_FAILED,
  NUM_DROPS
};
const char * const drop_names[NUM_DROPS] =
{
  "malformed", "too big", "unknown sender", "wrong key", "wrong address",
  "over the rate limit", "unverified address (answered)", "room's Live is off", "sender may not send",
  "no such player", "could not send",
};

struct Peer
{
  unsigned int room;
  unsigned int tcp_ip;
  WDL_UINT64 key;
  bool may_send;
  bool bound; // addr is where it last said HELLO from, with a cookie it was sent there
  bool fresh; // and that was less than FRESH_SECONDS ago; PeerBound() and Generation() follow it
  struct sockaddr_in addr;
  unsigned int local_ip; // the address of this machine that it sends to, 0 if not known
  double last_rx; // when it last did
  double tokens_bytes, tokens_packets, tokens_time; // rate limit; tokens_time < 0 until the first datagram
};

struct Room
{
  Room() : on(false), kbps(128), frame(240) { }
  bool on;
  int kbps, frame;
  std::vector<unsigned int> peers;
};

// where a datagram is forwarded to
struct Dest
{
  unsigned int id;
  struct sockaddr_in addr;
  unsigned int local_ip; // and from, see Peer
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

WDL_UINT64 rotl(WDL_UINT64 x, int b)
{
  return (x << b) | (x >> (64 - b));
}

void sip_round(WDL_UINT64 v[4])
{
  v[0] += v[1]; v[1] = rotl(v[1], 13); v[1] ^= v[0]; v[0] = rotl(v[0], 32);
  v[2] += v[3]; v[3] = rotl(v[3], 16); v[3] ^= v[2];
  v[0] += v[3]; v[3] = rotl(v[3], 21); v[3] ^= v[0];
  v[2] += v[1]; v[1] = rotl(v[1], 17); v[1] ^= v[2]; v[2] = rotl(v[2], 32);
}

// SipHash-2-4 (Aumasson and Bernstein): a keyed hash whose output can't be guessed or
// forged without the key, however many outputs one has seen
WDL_UINT64 siphash24(const unsigned char key[16], const unsigned char *in, size_t len)
{
  const WDL_UINT64 k0 = get_le64(key), k1 = get_le64(key + 8);
  WDL_UINT64 v[4] = { 0x736f6d6570736575ULL ^ k0, 0x646f72616e646f6dULL ^ k1, 0x6c7967656e657261ULL ^ k0, 0x7465646279746573ULL ^ k1 };
  WDL_UINT64 m;
  size_t i = 0;
  for (; i + 8 <= len; i += 8)
  {
    m = get_le64(in + i);
    v[3] ^= m;
    sip_round(v);
    sip_round(v);
    v[0] ^= m;
  }
  m = (WDL_UINT64)(len & 0xff) << 56;
  for (int j = 0; i + j < len; j ++) m |= (WDL_UINT64)in[i + j] << (8 * j);
  v[3] ^= m;
  sip_round(v);
  sip_round(v);
  v[0] ^= m;
  v[2] ^= 0xff;
  for (int j = 0; j < 4; j ++) sip_round(v);
  return v[0] ^ v[1] ^ v[2] ^ v[3];
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

// the address a datagram was sent to, if a reply can come from it: not a broadcast or
// multicast one (0 = let the routing table pick)
unsigned int reply_address(unsigned int local_ip)
{
  const unsigned int h = ntohl(local_ip);
  if (h == 0xffffffff || (h & 0xf0000000) == 0xe0000000) return 0;
  return local_ip;
}

// whether a send failed because of the address it was sent from, e.g. one that is no
// longer this machine's
bool bad_source_address(int err)
{
#ifdef _WIN32
  return err == WSAEINVAL || err == WSAEADDRNOTAVAIL;
#else
  return err == EINVAL || err == EADDRNOTAVAIL;
#endif
}

} // namespace

struct LiveRelay::State
{
  State() : sock(INVALID_SOCKET), port(0), pktinfo(false), stop(false), strict(true), generation(0)
  {
#ifdef LIVE_PKTINFO_WINSOCK
    wsarecvmsg = NULL;
#endif
  }

  SOCKET sock;
  int port;
  bool pktinfo; // the socket says which address each datagram was sent to (LIVE_PKTINFO)
#ifdef LIVE_PKTINFO_WINSOCK
  LPFN_WSARECVMSG wsarecvmsg;
#endif
  std::thread thread;
  std::atomic<bool> stop;

  std::mutex mutex; // guards what follows
  std::map<unsigned int, Peer> peers;
  std::map<unsigned int, Room> rooms;
  bool strict;

  std::atomic<unsigned int> generation;

  // the relay thread's own (set by Open before it starts)
  unsigned char secret[16]; // for the cookies, new on each Open
  double fresh_seconds; // FRESH_SECONDS, or less for the tests
  double stats_start, stats_seconds;
  unsigned long long received, sent, dropped[NUM_DROPS];

  // A HELLO_ACK's cookie: whoever echoes it gets what is sent to this address and port,
  // since that's where it went. Only the relay can make one, from its secret: for this
  // player and key, and for COOKIE_SECONDS from epoch (now / COOKIE_SECONDS).
  WDL_UINT64 Cookie(unsigned int id, WDL_UINT64 key, const struct sockaddr_in &addr, unsigned int epoch) const
  {
    unsigned char m[22];
    put_le32(m, id);
    put_le32(m + 4, (unsigned int)key);
    put_le32(m + 8, (unsigned int)(key >> 32));
    memcpy(m + 12, &addr.sin_addr.s_addr, 4);
    memcpy(m + 16, &addr.sin_port, 2);
    put_le32(m + 18, epoch);
    return siphash24(secret, m, sizeof(m));
  }

  void Run();
  int Receive(unsigned char *buf, int size, struct sockaddr_in *from, unsigned int *local_ip);
  void Handle(unsigned char *buf, int len, const struct sockaddr_in &from, unsigned int local_ip, double now);
  void Housekeeping(double now);
  bool SendTo(const unsigned char *buf, int len, const struct sockaddr_in &to, unsigned int local_ip);
  int SendFrom(const unsigned char *buf, int len, const struct sockaddr_in &to, unsigned int local_ip);
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

  // learn which address each datagram was sent to, to answer from it (see LIVE_PKTINFO);
  // without it, the routing table picks
  int on = 1;
  st->pktinfo = false;
#if defined(LIVE_PKTINFO) || defined(LIVE_PKTINFO_WINSOCK)
  st->pktinfo = !setsockopt(s, IPPROTO_IP, IP_PKTINFO, (const char *)&on, sizeof(on));
#elif defined(LIVE_RECVDSTADDR)
  st->pktinfo = !setsockopt(s, IPPROTO_IP, IP_RECVDSTADDR, (const char *)&on, sizeof(on));
#endif
#ifdef LIVE_PKTINFO_WINSOCK
  st->wsarecvmsg = NULL;
  if (st->pktinfo)
  {
    GUID guid = WSAID_WSARECVMSG;
    DWORD got = 0;
    if (WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &st->wsarecvmsg, sizeof(st->wsarecvmsg),
                 &got, NULL, NULL) || !st->wsarecvmsg)
      st->pktinfo = false;
  }
#endif
  (void)on;

  // the old cookies stop working, as do the keys of the players it forgets
  if (!random_bytes(st->secret, sizeof(st->secret)))
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
  st->fresh_seconds = FRESH_SECONDS;
  env = getenv("NINJAM_LIVE_FRESH_SECONDS");
  if (env && atof(env) > 0) st->fresh_seconds = atof(env);
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
        unsigned int local_ip;
        const int n = Receive(buf, sizeof(buf), &from, &local_ip);
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
        if (from.sin_family != AF_INET) continue;
        Handle(buf, n, from, reply_address(local_ip), now_seconds());
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

// Reads a datagram: returns its length, or < 0 (JNL_ERRNO says why). from gets where it
// came from (sin_family isn't AF_INET if that isn't an IPv4 address), local_ip the
// address of this machine it was sent to (0 if not known).
int LiveRelay::State::Receive(unsigned char *buf, int size, struct sockaddr_in *from, unsigned int *local_ip)
{
  memset(from, 0, sizeof(*from));
  *local_ip = 0;
  if (!pktinfo)
  {
    socklen_t fromlen = sizeof(*from);
    const int n = (int)recvfrom(sock, (char *)buf, size, 0, (struct sockaddr *)from, &fromlen);
    if (n >= 0 && fromlen < (socklen_t)sizeof(*from)) from->sin_family = AF_UNSPEC;
    return n;
  }

#ifdef LIVE_PKTINFO_WINSOCK
  WSABUF data;
  data.buf = (char *)buf;
  data.len = (ULONG)size;
  union { WSACMSGHDR align; char space[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))]; } control;
  WSAMSG msg;
  memset(&msg, 0, sizeof(msg));
  msg.name = (struct sockaddr *)from;
  msg.namelen = sizeof(*from);
  msg.lpBuffers = &data;
  msg.dwBufferCount = 1;
  msg.Control.buf = control.space;
  msg.Control.len = sizeof(control.space);
  DWORD n = 0;
  if (wsarecvmsg(sock, &msg, &n, NULL, NULL)) return -1; // WSAEMSGSIZE for one too big, as with recvfrom
  if (msg.namelen < (int)sizeof(*from)) from->sin_family = AF_UNSPEC;
  for (WSACMSGHDR *c = WSA_CMSG_FIRSTHDR(&msg); c; c = WSA_CMSG_NXTHDR(&msg, c))
  {
    if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO && c->cmsg_len >= WSA_CMSG_LEN(sizeof(IN_PKTINFO)))
    {
      IN_PKTINFO pi;
      memcpy(&pi, WSA_CMSG_DATA(c), sizeof(pi));
      *local_ip = pi.ipi_addr.s_addr;
    }
  }
  return (int)n;
#elif defined(LIVE_PKTINFO) || defined(LIVE_RECVDSTADDR)
  struct iovec data;
  data.iov_base = buf;
  data.iov_len = (size_t)size;
#ifdef LIVE_PKTINFO
  union { struct cmsghdr align; char space[CMSG_SPACE(sizeof(struct in_pktinfo))]; } control;
#else
  union { struct cmsghdr align; char space[CMSG_SPACE(sizeof(struct in_addr))]; } control;
#endif
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_name = from;
  msg.msg_namelen = sizeof(*from);
  msg.msg_iov = &data;
  msg.msg_iovlen = 1;
  msg.msg_control = control.space;
  msg.msg_controllen = sizeof(control.space);
  const int n = (int)recvmsg(sock, &msg, 0);
  if (n < 0) return n;
  if (msg.msg_namelen < (socklen_t)sizeof(*from)) from->sin_family = AF_UNSPEC;
  for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
  {
#ifdef LIVE_PKTINFO
    if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO && c->cmsg_len >= CMSG_LEN(sizeof(struct in_pktinfo)))
    {
      struct in_pktinfo pi;
      memcpy(&pi, CMSG_DATA(c), sizeof(pi));
      *local_ip = pi.ipi_addr.s_addr;
    }
#else
    if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVDSTADDR && c->cmsg_len >= CMSG_LEN(sizeof(struct in_addr)))
    {
      struct in_addr a;
      memcpy(&a, CMSG_DATA(c), sizeof(a));
      *local_ip = a.s_addr;
    }
#endif
  }
  return n;
#else
  return -1; // pktinfo is never set without a way to read it
#endif
}

bool LiveRelay::State::TakeTokens(Peer &p, const Room *room, int len, double now)
{
  const int kbps = room && room->kbps > 0 ? room->kbps : 128;
  const int frame = room && room->frame > 0 ? room->frame : 240;
  // Per sender, charged once for each datagram it sends (every datagram is copied to
  // ndest players, so what the server sends for it is ndest times this): twice its
  // stream, which is its Opus bitrate plus about 64 bytes of headers in each datagram
  // (ours 20, AOO's 16 to 32, Opus's few), plus room for HELLOs and AOO's own messages.
  // Small frames make many small datagrams, whose headers can be as big as the audio.
  // It doesn't grow with the room, though AOO's resends go to each listener: that would
  // let one sender aim the whole room's worth at a single player. At the default 8
  // players, it covers resends up to about 14% loss, where the datagrams run out too.
  const double frames_per_second = 48000.0 / frame;
  const double bytes_per_second = 2.0 * (kbps * 1000.0 / 8.0 + frames_per_second * DATAGRAM_OVERHEAD) + 8000.0;
  const double packets_per_second = 2.0 * frames_per_second + 50.0;
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

// Sends from local_ip, the address of this machine the player sends to, when it is known
// (else the routing table picks). Returns false if local_ip couldn't be used, e.g. it is
// no longer this machine's; then the datagram went out from the routing table's choice.
bool LiveRelay::State::SendTo(const unsigned char *data, int len, const struct sockaddr_in &to, unsigned int local_ip)
{
  bool local_ok = true;
  if (pktinfo && local_ip)
  {
    const int n = SendFrom(data, len, to, local_ip);
    if (n == len)
    {
      sent ++;
      return true;
    }
    if (n >= 0 || !bad_source_address(JNL_ERRNO))
    {
      dropped[DROP_SEND_FAILED] ++; // e.g. the send buffer is full
      return true;
    }
    local_ok = false;
  }
  if (sendto(sock, (const char *)data, len, 0, (const struct sockaddr *)&to, sizeof(to)) == len) sent ++;
  else dropped[DROP_SEND_FAILED] ++;
  return local_ok;
}

// sendto() from local_ip; returns what sendto() would
int LiveRelay::State::SendFrom(const unsigned char *data, int len, const struct sockaddr_in &to, unsigned int local_ip)
{
#ifdef LIVE_PKTINFO_WINSOCK
  WSABUF buf;
  buf.buf = (char *)data;
  buf.len = (ULONG)len;
  union { WSACMSGHDR align; char space[WSA_CMSG_SPACE(sizeof(IN_PKTINFO))]; } control;
  memset(&control, 0, sizeof(control));
  WSAMSG msg;
  memset(&msg, 0, sizeof(msg));
  msg.name = (struct sockaddr *)&to;
  msg.namelen = sizeof(to);
  msg.lpBuffers = &buf;
  msg.dwBufferCount = 1;
  msg.Control.buf = control.space;
  msg.Control.len = sizeof(control.space);
  WSACMSGHDR *c = WSA_CMSG_FIRSTHDR(&msg);
  c->cmsg_level = IPPROTO_IP;
  c->cmsg_type = IP_PKTINFO;
  c->cmsg_len = WSA_CMSG_LEN(sizeof(IN_PKTINFO));
  IN_PKTINFO pi;
  memset(&pi, 0, sizeof(pi));
  pi.ipi_addr.s_addr = local_ip; // the source address; interface 0: the routing table's
  memcpy(WSA_CMSG_DATA(c), &pi, sizeof(pi));
  DWORD n = 0;
  if (WSASendMsg(sock, &msg, 0, &n, NULL, NULL)) return -1;
  return (int)n;
#elif defined(LIVE_PKTINFO) || defined(LIVE_RECVDSTADDR)
  struct iovec buf;
  buf.iov_base = (void *)data;
  buf.iov_len = (size_t)len;
#ifdef LIVE_PKTINFO
  union { struct cmsghdr align; char space[CMSG_SPACE(sizeof(struct in_pktinfo))]; } control;
#else
  union { struct cmsghdr align; char space[CMSG_SPACE(sizeof(struct in_addr))]; } control;
#endif
  memset(&control, 0, sizeof(control));
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_name = (void *)&to;
  msg.msg_namelen = sizeof(to);
  msg.msg_iov = &buf;
  msg.msg_iovlen = 1;
  msg.msg_control = control.space;
  msg.msg_controllen = sizeof(control.space);
  struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
  c->cmsg_level = IPPROTO_IP;
#ifdef LIVE_PKTINFO
  c->cmsg_type = IP_PKTINFO;
  c->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));
  struct in_pktinfo pi;
  memset(&pi, 0, sizeof(pi));
  pi.ipi_spec_dst.s_addr = local_ip; // the source address; interface 0: the routing table's
  memcpy(CMSG_DATA(c), &pi, sizeof(pi));
#else
  c->cmsg_type = IP_SENDSRCADDR;
  c->cmsg_len = CMSG_LEN(sizeof(struct in_addr));
  struct in_addr a;
  a.s_addr = local_ip;
  memcpy(CMSG_DATA(c), &a, sizeof(a));
#endif
  return (int)sendmsg(sock, &msg, 0);
#else
  (void)local_ip;
  return (int)sendto(sock, (const char *)data, len, 0, (const struct sockaddr *)&to, sizeof(to));
#endif
}

void LiveRelay::State::Handle(unsigned char *buf, int len, const struct sockaddr_in &from, unsigned int local_ip, double now)
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

  Dest to[MAX_DESTS];
  int nto = 0;
  WDL_UINT64 cookie = 0; // for the HELLO_ACK
  bool verified = false;
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

    std::map<unsigned int, Room>::const_iterator r = rooms.find(p.room);
    const Room *room = r != rooms.end() ? &r->second : NULL;
    if (!TakeTokens(p, room, len, now))
    {
      dropped[DROP_RATE] ++;
      return;
    }

    if (type == TYPE_HELLO)
    {
      // Anyone can write another address on a datagram, and the relay would send the
      // room's audio there. So the player is at an address (its router may give it
      // another port) only once a HELLO from there echoes the cookie the relay sent
      // there, which shows that whoever sent it gets what is sent there. Only such a
      // HELLO keeps the player fresh: the cookie expires, its audio doesn't count.
      const unsigned int epoch = (unsigned int)(now / COOKIE_SECONDS);
      const WDL_UINT64 echoed = get_le64(buf + HELLO_COOKIE);
      cookie = Cookie(sender, key, from, epoch);
      verified = same_key(echoed, cookie) || same_key(echoed, Cookie(sender, key, from, epoch - 1));
      if (verified)
      {
        p.addr = from;
        p.local_ip = local_ip;
        p.bound = true;
        p.last_rx = now;
        if (!p.fresh)
        {
          p.fresh = true;
          generation ++;
        }
      }
    }
    else
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
        to[nto].id = dest;
        to[nto].addr = q->second.addr;
        to[nto ++].local_ip = q->second.local_ip;
      }
      else
      {
        for (size_t i = 0; i < room->peers.size() && nto < MAX_DESTS; i ++)
        {
          if (room->peers[i] == sender) continue;
          std::map<unsigned int, Peer>::const_iterator q = peers.find(room->peers[i]);
          if (q != peers.end() && q->second.may_send && q->second.bound && q->second.fresh)
          {
            to[nto].id = q->first;
            to[nto].addr = q->second.addr;
            to[nto ++].local_ip = q->second.local_ip;
          }
        }
      }
    }
  }

  if (type == TYPE_HELLO)
  {
    // answered either way, smaller than the HELLO, so nobody gets more than was sent
    // with their address on it
    unsigned char ack[HELLO_ACK_SIZE];
    ack[0] = 'N';
    ack[1] = 'L';
    ack[2] = UDP_VERSION;
    ack[3] = TYPE_HELLO;
    put_le32(ack + 4, 0);
    memcpy(ack + 8, buf + CLIENT_HEADER, 8); // the client's timestamp
    put_le32(ack + 16, (unsigned int)cookie);
    put_le32(ack + 20, (unsigned int)(cookie >> 32));
    if (!verified) dropped[DROP_UNVERIFIED] ++;
    SendTo(ack, sizeof(ack), from, local_ip); // from the address it was sent to
    return;
  }

  // the server's header goes right before the audio, which stays where it is
  unsigned char *out = buf + CLIENT_HEADER - SERVER_HEADER;
  out[0] = 'N';
  out[1] = 'L';
  out[2] = UDP_VERSION;
  out[3] = TYPE_DATA;
  put_le32(out + 4, sender);
  // each from the address of this machine its player sends to
  int failed = 0;
  for (int i = 0; i < nto; i ++)
  {
    if (!SendTo(out, len - (CLIENT_HEADER - SERVER_HEADER), to[i].addr, to[i].local_ip))
    {
      if (failed != i) to[failed] = to[i];
      failed ++;
    }
  }
  if (failed)
  {
    // that address is gone: the routing table picks until the player is heard from again
    std::lock_guard<std::mutex> lock(mutex);
    for (int i = 0; i < failed; i ++)
    {
      std::map<unsigned int, Peer>::iterator q = peers.find(to[i].id);
      if (q != peers.end() && q->second.local_ip == to[i].local_ip) q->second.local_ip = 0;
    }
  }
}

void LiveRelay::State::Housekeeping(double now)
{
  int players = 0;
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (std::map<unsigned int, Peer>::iterator it = peers.begin(); it != peers.end(); ++ it)
    {
      Peer &p = it->second;
      if (p.fresh && now - p.last_rx >= fresh_seconds)
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
