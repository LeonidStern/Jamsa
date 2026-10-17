/*
    Jamsa Live - liveclient.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Client: Core (livecore.cpp) plus a UDP socket connected to the server, a
  receive thread and a send thread.

  - receive thread: recv() with a 100 ms timeout; a HELLO_ACK gives the
    round trip to the server and the address cookie, a DATA datagram goes
    through Core's filter into AOO;
  - send thread (raised priority): sleeps on a semaphore for at most 10 ms;
    the audio thread posts it when AOO has something to send. Sends a HELLO
    at start, every 2 s, and when the server hands us a new cookie, then
    whatever AOO has (each AOO packet behind our 20-byte header).

  The audio thread's only system call is that post, once per block at most
  and only while the send thread hasn't run since (an atomic flag).

*/

// winsock2.h before anything that could pull in windows.h
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <pthread/qos.h>
#else
#include <semaphore.h>
#include <time.h>
#endif
#endif

#include "liveclient.h"
#include "livecore.h"

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <chrono>
#include <climits>
#include <mutex>
#include <thread>

namespace jamsa_live {

namespace {

#ifdef _WIN32
typedef SOCKET socket_t;
const socket_t kNoSocket = INVALID_SOCKET;
void closeSocket(socket_t s) { closesocket(s); }
int socketError() { return WSAGetLastError(); }
// a recv() error that isn't the end of the socket: timeout, an ICMP
// "port unreachable" from an earlier send, a datagram too large
bool transientError(int e)
{
  return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK || e == WSAECONNRESET || e == WSAEMSGSIZE
    || e == WSAEINTR || e == WSAENETRESET || e == WSAECONNREFUSED;
}
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
typedef int socket_t;
const socket_t kNoSocket = -1;
void closeSocket(socket_t s) { close(s); }
int socketError() { return errno; }
bool transientError(int e)
{
  return e == EAGAIN || e == EWOULDBLOCK || e == EINTR || e == ECONNREFUSED || e == ETIMEDOUT
    || e == EHOSTUNREACH || e == ENETUNREACH || e == EMSGSIZE;
}
#endif

double steadySeconds()
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// a HELLO's time stamp: microseconds on the steady clock, never 0
uint64_t steadyMicros()
{
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count();
  return (uint64_t)us | ((uint64_t)1 << 63);
}

// the send thread's wake-up: post() doesn't block or allocate
class Semaphore
{
public:
#ifdef _WIN32
  Semaphore() { m_sem = CreateSemaphoreW(nullptr, 0, LONG_MAX, nullptr); }
  ~Semaphore() { if (m_sem) CloseHandle(m_sem); }
  void post() { ReleaseSemaphore(m_sem, 1, nullptr); }
  void wait(int ms) { WaitForSingleObject(m_sem, (DWORD)ms); }
private:
  HANDLE m_sem;
#elif defined(__APPLE__)
  Semaphore() { m_sem = dispatch_semaphore_create(0); }
  ~Semaphore() { dispatch_release(m_sem); }
  void post() { dispatch_semaphore_signal(m_sem); }
  void wait(int ms) { dispatch_semaphore_wait(m_sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * 1000000)); }
private:
  dispatch_semaphore_t m_sem;
#else
  Semaphore() { sem_init(&m_sem, 0, 0); }
  ~Semaphore() { sem_destroy(&m_sem); }
  void post() { sem_post(&m_sem); }
  void wait(int ms)
  {
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
    // on the steady clock: a wall clock step can't stall the HELLOs
    const clockid_t clock = CLOCK_MONOTONIC;
#else
    const clockid_t clock = CLOCK_REALTIME;
#endif
    timespec ts;
    clock_gettime(clock, &ts);
    ts.tv_nsec += (long)ms * 1000000;
    ts.tv_sec += ts.tv_nsec / 1000000000;
    ts.tv_nsec %= 1000000000;
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 30))
    sem_clockwait(&m_sem, clock, &ts);
#else
    sem_timedwait(&m_sem, &ts);
#endif
  }
private:
  sem_t m_sem;
#endif
};

// The send thread above normal priority, best effort (a plug-in usually
// can't get real-time scheduling on Linux; it keeps the normal one then).
bool raiseThreadPriority()
{
#ifdef _WIN32
  return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL) != 0;
#elif defined(__APPLE__)
  return pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
#else
  // below a typical audio thread (JACK, PipeWire: 70..88), above everything else
  for (int policy : { SCHED_FIFO, SCHED_RR })
  {
    sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = sched_get_priority_min(policy) + 9;
    if (pthread_setschedparam(pthread_self(), policy, &sp) == 0) return true;
  }
  return false;
#endif
}

// ---------------------------------------------------------------------------
// AOO's log: it may log on the audio thread, so its messages go into a
// small ring (try-lock: a message that would have to wait is counted and
// dropped) and poll() writes them out. One ring for the process, like
// AOO's log function.

struct LogRing
{
  std::mutex mutex;
  char lines[16][256];
  int levels[16];
  int head = 0, count = 0;
  std::atomic<uint64_t> lost{0};
};

LogRing &logRing()
{
  static LogRing ring;
  return ring;
}

void AOO_CALL logToRing(AooLogLevel level, const AooChar *message)
{
  LogRing &r = logRing();
  std::unique_lock<std::mutex> lock(r.mutex, std::try_to_lock);
  if (!lock.owns_lock() || r.count == 16)
  {
    r.lost++;
    return;
  }
  const int i = (r.head + r.count) % 16;
  snprintf(r.lines[i], sizeof(r.lines[i]), "%s", message ? message : "");
  r.levels[i] = (int)level;
  r.count++;
}

void drainLog()
{
  LogRing &r = logRing();
  char line[256];
  for (;;)
  {
    {
      std::lock_guard<std::mutex> lock(r.mutex);
      if (!r.count) break;
      memcpy(line, r.lines[r.head], sizeof(line));
      r.head = (r.head + 1) % 16;
      r.count--;
    }
    fprintf(stderr, "[jamsa live] %s\n", line);
  }
  const uint64_t lost = r.lost.exchange(0);
  if (lost) fprintf(stderr, "[jamsa live] (%llu log messages dropped)\n", (unsigned long long)lost);
}

void installLog()
{
  static std::once_flag once;
  std::call_once(once, [] { aoo_setLogFunction(logToRing); });
}

} // namespace

// ---------------------------------------------------------------------------

struct Client::Impl
{
  Core core;

  // control thread
  Config cfg;
  std::vector<Peer> peers;           // for a restart
  std::atomic<bool> running{false};
#ifdef _WIN32
  bool wsa = false;
#endif

  // fixed while the threads run
  socket_t sock = kNoSocket;
  uint32_t myId = 0;
  uint64_t key = 0;
  std::thread rx, tx;
  std::atomic<bool> quit{false};

  // the send thread's wake-up
  Semaphore wake;
  std::atomic<bool> wakePending{false};
  std::atomic<bool> helloNow{false};

  // HELLO / HELLO_ACK
  std::atomic<uint64_t> stamps[8];   // the last HELLOs' time stamps (send thread)
  int stampNext = 0;
  std::atomic<uint64_t> cookie{0};   // the latest valid one the server sent
  std::atomic<int64_t> rttMicros{-1};

  // counters
  std::atomic<uint64_t> sent{0}, received{0}, droppedBad{0}, helloAcks{0};
  std::atomic<bool> realtime{false};

  Impl()
  {
    for (auto &s : stamps) s.store(0);
    installLog();
  }

  void signal()
  {
    if (!wakePending.exchange(true, std::memory_order_acq_rel)) wake.post();
  }

  void sendDatagram(const uint8_t *data, size_t size)
  {
#ifdef _WIN32
    const int r = ::send(sock, (const char *)data, (int)size, 0);
#else
    const ssize_t r = ::send(sock, data, size, 0);
#endif
    if (r >= 0 && (size_t)r == size) sent++;
  }

  static void sendData(void *user, uint32_t dest, const uint8_t *data, size_t size)
  {
    Impl *self = (Impl *)user;
    uint8_t buf[wire::kMaxDatagram];
    if (size == 0 || size > wire::kMaxPayload) return;
    wire::writeDataHeader(buf, self->myId, self->key, dest);
    memcpy(buf + wire::kClientHeaderSize, data, size);
    self->sendDatagram(buf, wire::kClientHeaderSize + size);
  }

  void sendHello()
  {
    const uint64_t stamp = steadyMicros();
    stamps[stampNext].store(stamp);
    stampNext = (stampNext + 1) % 8;
    uint8_t buf[wire::kHelloSize];
    const size_t n = wire::writeHello(buf, myId, key, stamp, cookie.load());
    sendDatagram(buf, n);
  }

  void handleAck(const wire::FromServer &m)
  {
    // only an answer to one of our recent HELLOs counts: a forged ACK
    // from our address can't change our cookie
    const uint64_t now = steadyMicros();
    bool ours = false;
    for (const auto &s : stamps)
      if (s.load() == m.stamp && m.stamp != 0) ours = true;
    const int64_t rtt = (int64_t)(now - m.stamp);
    if (!ours || rtt < 0 || rtt > 10 * 1000000)
    {
      droppedBad++;
      return;
    }
    helloAcks++;
    rttMicros.store(rtt);
    // a new cookie (the first, a new address, the server's next epoch):
    // echo it at once, so that the server binds us within one round trip
    if (m.cookie != cookie.load())
    {
      cookie.store(m.cookie);
      helloNow.store(true);
      signal();
    }
  }

  void receiveLoop()
  {
    uint8_t buf[wire::kMaxDatagram + 1];
    while (!quit.load(std::memory_order_acquire))
    {
#ifdef _WIN32
      const int n = ::recv(sock, (char *)buf, (int)sizeof(buf), 0);
#else
      const ssize_t n = ::recv(sock, buf, sizeof(buf), 0);
#endif
      if (n < 0)
      {
        const int e = socketError();
        if (!transientError(e)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
#ifdef _WIN32
        if (e == WSAEMSGSIZE) { received++; droppedBad++; }
#endif
        continue;
      }
      if (quit.load(std::memory_order_acquire)) break;
      received++;
      wire::FromServer m;
      if ((size_t)n > wire::kMaxDatagram || !wire::parseFromServer(buf, (size_t)n, m))
      {
        droppedBad++;
        continue;
      }
      if (m.type == wire::kHelloAck) handleAck(m);
      else core.handleData(m.from, m.payload, m.size, ntpFromSeconds(steadySeconds())); // processBlock()'s clock
    }
  }

  void sendLoop()
  {
    realtime.store(raiseThreadPriority());
    double nextHello = 0;
    while (!quit.load(std::memory_order_acquire))
    {
      wakePending.store(false, std::memory_order_release);
      const double now = steadySeconds();
      if (helloNow.exchange(false) || now >= nextHello)
      {
        sendHello();
        nextHello = now + 2.0;
      }
      core.send(sendData, this);
      wake.wait(10);
    }
  }

  bool open(const Config &c, std::string *error)
  {
    auto fail = [&](const char *what) {
      if (error)
      {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s (error %d)", what, socketError());
        *error = buf;
      }
      if (sock != kNoSocket) closeSocket(sock);
      sock = kNoSocket;
      return false;
    };
#ifdef _WIN32
    if (!wsa)
    {
      WSADATA wd;
      if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
      {
        if (error) *error = "WSAStartup failed";
        return false;
      }
      wsa = true;
    }
#endif
    sock = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kNoSocket) return fail("can't open a UDP socket");
#ifdef SO_NOSIGPIPE
    // stop() shuts the socket down while the send thread may be in send():
    // on macOS that send raises SIGPIPE, which ends the host unless it
    // ignores the signal (Linux only fails it with EPIPE)
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    // big enough for a few hundred ms of the whole room
    int bufsize = 256 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char *)&bufsize, sizeof(bufsize));
#ifdef _WIN32
    DWORD timeout = 100;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    // no WSAECONNRESET on recv() for an ICMP "port unreachable"
    BOOL reset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(sock, SIO_UDP_CONNRESET, &reset, sizeof(reset), nullptr, 0, &bytes, nullptr, nullptr);
#else
    timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    sockaddr_in server;
    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_addr.s_addr = c.serverIp;
    server.sin_port = htons(c.udpPort);
    if (::connect(sock, (const sockaddr *)&server, sizeof(server)) != 0)
      return fail("can't connect the UDP socket to the server");
    return true;
  }

  void apply(const Config &c)
  {
    core.configure(c.kbps, c.frameSamples);
    core.setRoomOn(c.roomOn);
    core.setStreaming(c.roomOn && c.maySend);
  }

  bool start(const Config &c, std::string *error)
  {
    if (!c.udpOpen)
    {
      if (error) *error = "the server takes no Live UDP";
      return false;
    }
    if (!c.myId || !c.serverIp || !c.udpPort)
    {
      if (error) *error = "no peer ID or server address";
      return false;
    }
    if (!open(c, error)) return false;
    cfg = c;
    myId = c.myId;
    key = c.key;
    cookie.store(0);
    rttMicros.store(-1);
    for (auto &s : stamps) s.store(0);
    core.setSelf(c.myId);
    apply(c);
    quit.store(false);
    helloNow.store(true);
    wakePending.store(false);
    rx = std::thread([this] { receiveLoop(); });
    tx = std::thread([this] { sendLoop(); });
    running.store(true);
    return true;
  }

  void stop()
  {
    if (!running.load()) return;
    core.setStreaming(false);
    quit.store(true, std::memory_order_release);
    wake.post();
#ifdef _WIN32
    shutdown(sock, SD_BOTH);
#else
    shutdown(sock, SHUT_RDWR);
#endif
    if (rx.joinable()) rx.join();
    if (tx.joinable()) tx.join();
    closeSocket(sock);
    sock = kNoSocket;
    core.setRoomOn(false);
    core.setPeers(std::vector<Peer>());
    running.store(false);
  }

  ~Impl()
  {
    stop();
#ifdef _WIN32
    if (wsa) WSACleanup();
#endif
  }
};

// ---------------------------------------------------------------------------

Client::Client() : m_impl(new Impl)
{
}

Client::~Client()
{
}

bool Client::start(const Config &c, std::string *error)
{
  // the players set before (stop() forgets them, a stop from update()
  // doesn't): a start after that, or while running, keeps them
  Impl &d = *m_impl;
  d.stop();
  if (!d.start(c, error)) return false;
  d.core.setPeers(d.peers);
  return true;
}

void Client::update(const Config &c)
{
  Impl &d = *m_impl;
  if (!d.running.load()) return;
  if (!c.udpOpen)
  {
    d.stop();
    return;
  }
  if (c.myId != d.cfg.myId || c.key != d.cfg.key || c.serverIp != d.cfg.serverIp
      || c.udpPort != d.cfg.udpPort)
  {
    // another server or another identity: a new socket and new HELLOs
    d.stop();
    if (d.start(c, nullptr)) d.core.setPeers(d.peers);
    return;
  }
  d.cfg = c;
  d.apply(c);
}

void Client::setPeers(const std::vector<Peer> &peers)
{
  m_impl->peers = peers;
  m_impl->core.setPeers(peers);
}

void Client::setJitterMs(int ms)
{
  m_impl->core.setJitterMs(ms);
}

void Client::stop()
{
  m_impl->stop();
  m_impl->peers.clear();
}

bool Client::running() const
{
  return m_impl->running.load();
}

void Client::poll()
{
  m_impl->core.poll(steadySeconds());
  drainLog();
}

std::vector<PeerStats> Client::stats() const
{
  return m_impl->core.stats(steadySeconds());
}

double Client::serverRttMs() const
{
  const int64_t us = m_impl->rttMicros.load();
  return us < 0 ? 0 : us / 1000.0;
}

Counters Client::counters() const
{
  const Impl &d = *m_impl;
  const Core::Totals &t = d.core.totals();
  Counters c;
  c.sent = d.sent.load();
  c.received = d.received.load();
  c.accepted = t.accepted.load();
  c.droppedBad = d.droppedBad.load() + t.badSize.load();
  c.droppedPeer = t.noSlot.load();
  c.droppedFilter = t.filtered.load() + t.badFormat.load();
  c.droppedIdle = t.idle.load();
  c.helloAcks = d.helloAcks.load();
  c.sendThreadRealtime = d.realtime.load();
  return c;
}

void Client::prepare(double sampleRate, int maxBlock)
{
  m_impl->core.prepare(sampleRate, maxBlock);
}

void Client::processBlock(const float *inL, const float *inR, int frames, bool send)
{
  Impl &d = *m_impl;
  // AOO's clocks run on the steady clock (see README.md)
  const uint64_t t = ntpFromSeconds(steadySeconds());
  if (d.core.processBlock(inL, inR, frames, send, t)) d.signal();
}

int Client::slotOf(uint32_t peerId) const
{
  return m_impl->core.slotOf(peerId);
}

bool Client::slotHealthy(int slot) const
{
  return m_impl->core.slotHealthy(slot);
}

const float *Client::stem(int slot, int channel) const
{
  return m_impl->core.stem(slot, channel);
}

} // namespace jamsa_live
