/*
    Jamsa Live - live_client_test.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  jamsa_live::Client on real sockets: four players (Clients, each with an
  audio thread paced by the clock, as an audio interface would call it) and
  a minimal server on the loopback interface that speaks the UDP protocol
  (README.md): HELLO / HELLO_ACK with the address cookie, DATA copied to the
  room with its header rewritten in place. The main thread is the engine's
  network thread: setPeers(), poll(), stats().

  Checked: binding through the cookie within a round trip, every player
  hears the others (tones, SNR) and how late (bursts, against the
  statistics), lost copies resent, a forged HELLO_ACK ignored, the room's
  flag off and on, a player who leaves and one who takes the slot, and a
  sender and a listener whose clocks drift - all at once, with 44.1 and
  48 kHz players. Takes about 20 s. Build it with
  -fsanitize=thread to check the threads.

*/

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <timeapi.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "liveclient.h"
#include "livecore.h" // wire::, for the server
#include "testutil.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace jamsa_live;

// Under ThreadSanitizer everything runs several times slower and the audio
// threads miss their deadlines: the checks on timing (delays, dropouts,
// clean audio) only print there. The threads are what that build checks.
#if defined(__SANITIZE_THREAD__)
#define LIVE_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LIVE_TSAN 1
#endif
#endif
#ifdef LIVE_TSAN
static const bool g_timing = false;
#else
static const bool g_timing = true;
#endif
#define CHECK_TIMING(cond) \
  do { if (g_timing) CHECK(cond); else if (!(cond)) printf("  (timing, not checked here: %s)\n", #cond); } while (0)

#ifdef _WIN32
typedef SOCKET socket_t;
static const socket_t kNoSocket = INVALID_SOCKET;
static void close_socket(socket_t s) { closesocket(s); }
#else
typedef int socket_t;
static const socket_t kNoSocket = -1;
static void close_socket(socket_t s) { close(s); }
#endif

typedef std::chrono::steady_clock clock_type;

static double seconds_now()
{
  return std::chrono::duration<double>(clock_type::now().time_since_epoch()).count();
}

static void sleep_ms(int ms)
{
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---------------------------------------------------------------------------
// The server: what ninjam/server does for Live, minus the TCP side and the
// rate limits. One room; the test says who is in it.
// ---------------------------------------------------------------------------

struct fake_server
{
  struct peer
  {
    uint32_t id = 0;
    uint64_t key = 0;
    bool in_room = true;
    bool bound = false;
    sockaddr_in ep;
    double bound_at = 0;
    double loss = 0; // of the copies to it
  };

  socket_t sock = kNoSocket;
  uint16_t port = 0;
  std::thread thread;
  std::atomic<bool> quit{false};
  std::mutex mutex;
  std::vector<peer> peers;
  uint64_t secret = 0x243f6a8885a308d3ull;
  std::mt19937 rng{3};
  std::atomic<long> hellos{0}, verified{0}, unverified{0}, data_in{0}, data_out{0}, bad{0}, lost{0};

  bool open()
  {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == kNoSocket) return false;
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    if (bind(sock, (const sockaddr *)&a, sizeof(a)) != 0) return false;
    socklen_t len = sizeof(a);
    getsockname(sock, (sockaddr *)&a, &len);
    port = ntohs(a.sin_port);
#ifdef _WIN32
    DWORD timeout = 50;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
#else
    timeval tv = { 0, 50000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    int bufsize = 1 << 20;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&bufsize, sizeof(bufsize));
    thread = std::thread([this] { loop(); });
    return true;
  }

  void close()
  {
    quit.store(true);
    if (thread.joinable()) thread.join();
    if (sock != kNoSocket) close_socket(sock);
    sock = kNoSocket;
  }

  void add(uint32_t id, uint64_t key)
  {
    std::lock_guard<std::mutex> lock(mutex);
    peer p;
    p.id = id;
    p.key = key;
    memset(&p.ep, 0, sizeof(p.ep));
    peers.push_back(p);
  }

  template <class F> void with(uint32_t id, F f)
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (peer &p : peers) if (p.id == id) f(p);
  }

  bool bound(uint32_t id, double *at = nullptr)
  {
    bool b = false;
    with(id, [&](peer &p) { b = p.bound; if (at) *at = p.bound_at; });
    return b;
  }

  // (a stand-in for the real SipHash cookie: as unguessable as a test needs)
  uint64_t cookie_for(const peer &p, const sockaddr_in &from) const
  {
    uint64_t x = secret ^ ((uint64_t)p.id << 32) ^ p.key ^ ((uint64_t)from.sin_addr.s_addr << 16) ^ from.sin_port;
    for (int i = 0; i < 3; i++)
    {
      x ^= x >> 31;
      x *= 0x9E3779B97F4A7C15ull;
    }
    return x | 1;
  }

  void send_to(const sockaddr_in &to, const uint8_t *data, size_t size)
  {
    sendto(sock, (const char *)data, (int)size, 0, (const sockaddr *)&to, sizeof(to));
  }

  void loop()
  {
    uint8_t buf[wire::kMaxDatagram + 1];
    while (!quit.load())
    {
      sockaddr_in from;
      socklen_t len = sizeof(from);
      const int n = (int)recvfrom(sock, (char *)buf, (int)sizeof(buf), 0, (sockaddr *)&from, &len);
      if (n <= 0) continue;
      wire::FromClient m;
      if (n > (int)wire::kMaxDatagram || !wire::parseFromClient(buf, (size_t)n, m))
      {
        bad++;
        continue;
      }
      std::lock_guard<std::mutex> lock(mutex);
      peer *s = nullptr;
      for (peer &p : peers) if (p.id == m.sender) s = &p;
      if (!s || s->key != m.key)
      {
        bad++;
        continue;
      }
      if (m.type == wire::kHello)
      {
        hellos++;
        const uint64_t c = cookie_for(*s, from);
        if (m.cookie == c)
        {
          // only a HELLO that echoes its address's cookie binds
          if (!s->bound) s->bound_at = seconds_now();
          s->bound = true;
          s->ep = from;
          verified++;
        }
        else unverified++;
        uint8_t ack[wire::kHelloAckSize];
        send_to(from, ack, wire::writeHelloAck(ack, m.stamp, c));
        continue;
      }
      data_in++;
      if (!s->in_room) continue;
      // the server's header in place, at offset 12
      wire::writeServerHeader(buf + 12, wire::kData, m.sender);
      for (peer &q : peers)
      {
        if (q.id == m.sender || !q.in_room || !q.bound || (m.dest && q.id != m.dest)) continue;
        if (q.loss > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < q.loss)
        {
          lost++;
          continue;
        }
        send_to(q.ep, buf + 12, (size_t)n - 12);
        data_out++;
      }
    }
  }
};

// ---------------------------------------------------------------------------
// A player: a Client and its audio thread
// ---------------------------------------------------------------------------

struct player
{
  uint32_t id = 0;
  Config cfg;
  Client client;
  double rate = 48000;
  int block = 256;
  double ppm = 0;            // its audio clock against the steady clock
  double tone_hz = 440;
  bool bursts = false;       // Hann bursts of 1 kHz every 200 ms instead of the tone
  std::thread thread;
  std::atomic<bool> quit{false};
  std::atomic<uint64_t> blocks{0};
  double start = 0;          // steady time of block 0
  // how late the calls came (seconds): the first one, on average, at most
  std::atomic<double> late_first{0}, late_mean{0}, late_max{0};

  // recording (the audio thread writes, the main thread reads what it has counted)
  static const size_t kMaxBlocks = 1000;
  std::atomic<bool> recording{false};
  std::atomic<size_t> rec_blocks{0};
  std::vector<float> rec[Client::MAX_PEERS];
  std::vector<double> rec_time;

  double eff_rate() const { return rate * (1 + ppm * 1e-6); }

  void input(int64_t index, float *l, float *r, int n)
  {
    for (int i = 0; i < n; i++)
    {
      double v;
      if (bursts)
      {
        const int64_t P = (int64_t)llround(0.2 * rate), L = (int64_t)llround(0.01 * rate);
        const int64_t k = (index + i) % P;
        v = k < L ? 0.5 * (0.5 - 0.5 * cos(6.283185307179586 * (double)k / (double)L))
                      * sin(6.283185307179586 * 1000.0 * (double)k / rate)
                  : 0.0;
      }
      else v = 0.5 * sin(6.283185307179586 * tone_hz * (double)(index + i) / rate);
      l[i] = (float)v;
      r[i] = (float)v;
    }
  }

  void audio()
  {
    std::vector<float> l(block), r(block);
    client.prepare(rate, block);
    uint64_t n = 0;
    double late_sum = 0, late_top = 0;
    while (!quit.load())
    {
      // when the "interface" wants this block; late wake-ups catch up
      const double due = start + (double)n * block / eff_rate();
      const double wait = due - seconds_now();
      if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
      const double late = seconds_now() - due;
      late_sum += late;
      late_top = std::max(late_top, late);
      if (n == 0) late_first.store(late);
      late_mean.store(late_sum / (double)(n + 1));
      late_max.store(late_top);
      input((int64_t)n * block, l.data(), r.data(), block);
      client.processBlock(l.data(), r.data(), block, true);
      const size_t b = rec_blocks.load(std::memory_order_relaxed);
      if (recording.load(std::memory_order_acquire) && b < kMaxBlocks)
      {
        for (int k = 0; k < Client::MAX_PEERS; k++)
        {
          const float *s = client.stem(k, 0);
          memcpy(rec[k].data() + b * block, s, block * sizeof(float));
        }
        rec_time[b] = due;
        rec_blocks.store(b + 1, std::memory_order_release);
      }
      n++;
      blocks.store(n, std::memory_order_release);
    }
  }

  void run_audio()
  {
    for (auto &x : rec) x.assign(kMaxBlocks * block, 0.0f);
    rec_time.assign(kMaxBlocks, 0.0);
    start = seconds_now() + 0.01;
    thread = std::thread([this] { audio(); });
  }

  void stop_audio()
  {
    quit.store(true);
    if (thread.joinable()) thread.join();
  }
};

// ---------------------------------------------------------------------------
// Signal checks
// ---------------------------------------------------------------------------

static double tone_snr(const float *x, size_t n, double hz, double sr)
{
  // a*sin + b*cos + c by least squares; tone power over what is left
  double m[3][4] = {};
  const double w = 6.283185307179586 * hz / sr;
  for (size_t i = 0; i < n; i++)
  {
    const double v[3] = { sin(w * (double)i), cos(w * (double)i), 1.0 };
    for (int r = 0; r < 3; r++)
    {
      for (int c = 0; c < 3; c++) m[r][c] += v[r] * v[c];
      m[r][3] += v[r] * x[i];
    }
  }
  for (int c = 0; c < 3; c++)
  {
    int p = c;
    for (int r = c + 1; r < 3; r++) if (fabs(m[r][c]) > fabs(m[p][c])) p = r;
    for (int k = 0; k < 4; k++) std::swap(m[c][k], m[p][k]);
    if (fabs(m[c][c]) < 1e-12) return -100;
    for (int r = 0; r < 3; r++)
    {
      if (r == c) continue;
      const double f = m[r][c] / m[c][c];
      for (int k = c; k < 4; k++) m[r][k] -= f * m[c][k];
    }
  }
  const double a = m[0][3] / m[0][0], b = m[1][3] / m[1][1], dc = m[2][3] / m[2][2];
  double sig = 0, res = 0;
  for (size_t i = 0; i < n; i++)
  {
    const double t = a * sin(w * (double)i) + b * cos(w * (double)i);
    sig += t * t;
    res += (x[i] - t - dc) * (x[i] - t - dc);
  }
  if (res <= 0) return 200;
  return 10 * log10(sig / res);
}

struct recording
{
  std::vector<float> samples;
  std::vector<double> time; // per block
  int block = 0;
  double rate = 0;
  double sample_time(size_t i) const { return time[i / block] + (double)(i % block) / rate; }
};

// the share of 20 ms windows in which p's tone comes through clean
static double clean_share(const recording &r, double hz)
{
  const size_t w = (size_t)(0.02 * r.rate);
  size_t windows = 0, good = 0;
  for (size_t i = 0; i + w <= r.samples.size(); i += w)
  {
    windows++;
    if (tone_snr(r.samples.data() + i, w, hz, r.rate) > 15) good++;
  }
  return windows ? (double)good / windows : 0;
}

// the bursts' delays: the centre of their energy against the centre of the
// burst the sender played, both on the steady clock
static std::vector<double> burst_delays(const player &src, const recording &r)
{
  std::vector<double> out;
  const int64_t P = (int64_t)llround(0.2 * src.rate), L = (int64_t)llround(0.01 * src.rate);
  const double first = src.start + (double)(L / 2) / src.eff_rate();
  const double step = (double)P / src.eff_rate();
  std::vector<double> e, et;
  for (size_t i = 0; i < r.samples.size(); i++)
  {
    const double t = r.sample_time(i);
    const long m = (long)floor((t - first) / step);
    if (m < 0) continue;
    if ((size_t)m >= e.size()) { e.resize(m + 1, 0); et.resize(m + 1, 0); }
    const double x2 = (double)r.samples[i] * r.samples[i];
    e[m] += x2;
    et[m] += x2 * (t - (first + m * step));
  }
  double emax = 0;
  for (double v : e) emax = std::max(emax, v);
  for (size_t m = 1; m + 1 < e.size(); m++)
    if (e[m] > 0.2 * emax) out.push_back(et[m] / e[m]);
  return out;
}

static double median(std::vector<double> v)
{
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

// ---------------------------------------------------------------------------

struct room
{
  fake_server server;
  std::vector<std::unique_ptr<player>> players;

  player &add(uint32_t id, double rate, int block, double tone_hz)
  {
    std::unique_ptr<player> p(new player);
    p->id = id;
    p->rate = rate;
    p->block = block;
    p->tone_hz = tone_hz;
    p->cfg.myId = id;
    p->cfg.key = 0x5bd1e9955bd1e995ull * id;
    p->cfg.serverIp = htonl(INADDR_LOOPBACK);
    p->cfg.udpPort = server.port;
    p->cfg.kbps = 128;
    p->cfg.frameSamples = 240;
    p->cfg.udpOpen = p->cfg.roomOn = p->cfg.maySend = true;
    server.add(id, p->cfg.key);
    players.push_back(std::move(p));
    return *players.back();
  }

  player &get(uint32_t id)
  {
    for (auto &p : players) if (p->id == id) return *p;
    return *players.front();
  }

  static std::vector<Peer> peers(std::initializer_list<uint32_t> ids)
  {
    std::vector<Peer> out;
    for (uint32_t id : ids)
    {
      Peer p;
      p.id = id;
      p.name = "p" + std::to_string(id);
      p.bound = true;
      out.push_back(p);
    }
    return out;
  }

  // the engine's network thread: poll every 30 ms
  void run(double seconds)
  {
    const double end = seconds_now() + seconds;
    while (seconds_now() < end)
    {
      for (auto &p : players) if (p->client.running()) p->client.poll();
      sleep_ms(30);
    }
  }

  template <class F> bool run_until(F done, double timeout)
  {
    const double end = seconds_now() + timeout;
    while (seconds_now() < end)
    {
      if (done()) return true;
      run(0.03);
    }
    return done();
  }

  bool healthy(uint32_t listener, uint32_t from)
  {
    player &l = get(listener);
    return l.client.slotHealthy(l.client.slotOf(from));
  }

  // records every listed player for a while; what each heard on 'from's slot
  void record(double seconds)
  {
    for (auto &p : players)
    {
      p->rec_blocks.store(0);
      p->recording.store(true, std::memory_order_release);
    }
    run(seconds);
    for (auto &p : players) p->recording.store(false);
    // until every audio thread is past its last recorded block
    std::vector<uint64_t> at;
    for (auto &p : players) at.push_back(p->blocks.load());
    for (size_t i = 0; i < players.size(); i++)
    {
      const double end = seconds_now() + 0.5;
      while (players[i]->thread.joinable() && players[i]->blocks.load() < at[i] + 2 && seconds_now() < end) sleep_ms(2);
    }
  }

  recording heard(uint32_t listener, uint32_t from)
  {
    player &l = get(listener);
    recording r;
    r.block = l.block;
    r.rate = l.eff_rate();
    const int slot = l.client.slotOf(from);
    if (slot < 0) return r;
    const size_t n = l.rec_blocks.load(std::memory_order_acquire);
    r.samples.assign(l.rec[slot].begin(), l.rec[slot].begin() + n * l.block);
    r.time.assign(l.rec_time.begin(), l.rec_time.begin() + n);
    return r;
  }
};

static const PeerStats *stats_of(const std::vector<PeerStats> &all, uint32_t id)
{
  for (const PeerStats &s : all) if (s.id == id) return &s;
  return nullptr;
}

static void test_start_errors()
{
  printf("start_errors\n");
  Client c;
  std::string error;
  Config cfg;
  cfg.myId = 1;
  cfg.serverIp = htonl(INADDR_LOOPBACK);
  cfg.udpPort = 9;
  CHECK(!c.start(cfg, &error)); // the server takes no UDP
  CHECK(!error.empty());
  cfg.udpOpen = true;
  cfg.myId = 0;
  error.clear();
  CHECK(!c.start(cfg, &error));
  CHECK(!error.empty());
  CHECK(!c.running());
  // without start(): silence, nothing bound
  c.prepare(48000, 64);
  float in[64] = {};
  c.processBlock(in, nullptr, 64, true);
  CHECK_EQ(c.slotOf(5), -1);
  CHECK(!c.slotHealthy(0));
  CHECK(c.stem(0, 0)[63] == 0);
  CHECK(c.stem(99, 3) != nullptr);
  CHECK_EQ(c.serverRttMs(), 0.0);
  c.stop();
}

// A Client that stops because the server's UDP closed (update() with
// udpOpen false) and starts again, or starts while running, keeps the
// players set before: the engine sends them again only when they change.
static void test_restart()
{
  printf("restart\n");
  room r;
  CHECK(r.server.open());
  player &a = r.add(1, 48000, 256, 500);
  player &b = r.add(2, 48000, 256, 700);
  player &c = r.add(3, 48000, 256, 900);
  a.client.setPeers(room::peers({ 2, 3 }));
  b.client.setPeers(room::peers({ 1, 3 }));
  c.client.setPeers(room::peers({ 1, 2 }));
  // (40 ms, as in test_room: this test is about restarts, and on a slow
  // machine, or under TSan, 20 ms without real-time priority drops out)
  for (auto &p : r.players) p->client.setJitterMs(40);
  std::string error;
  for (player *p : { &a, &b, &c })
  {
    CHECK(p->client.start(p->cfg, &error));
    p->run_audio();
  }
  auto all = [&] {
    return r.healthy(1, 2) && r.healthy(1, 3) && r.healthy(2, 1) && r.healthy(3, 1);
  };
  CHECK(r.run_until(all, 6.0));

  for (int round = 0; round < 2; round++)
  {
    if (round == 0)
    {
      // the server's UDP closes, then opens again
      a.cfg.udpOpen = false;
      a.client.update(a.cfg);
      CHECK(!a.client.running());
      a.cfg.udpOpen = true;
    }
    const long in = r.server.data_in.load();
    const double t0 = seconds_now();
    CHECK(a.client.start(a.cfg, &error)); // (round 1: while running)
    CHECK(a.client.running());
    CHECK(a.client.slotOf(2) >= 0 && a.client.slotOf(3) >= 0);
    CHECK_EQ(a.client.stats().size(), (size_t)2);
    const bool back = r.run_until(all, 6.0);
    printf("  %s: heard both ways again after %.2f s (%ld DATA at the server)\n",
           round == 0 ? "after a stop from update()" : "start() while running", seconds_now() - t0,
           r.server.data_in.load() - in);
    CHECK(back);
  }

  // C stops (its audio thread still runs, as in a host): A and B see its
  // stream end at once, without an underrun (a dropout in the statistics)
  auto streaming = [&](player &l, uint32_t from, int *underruns) {
    const std::vector<PeerStats> st = l.client.stats();
    const PeerStats *ps = stats_of(st, from);
    if (ps && underruns) *underruns = ps->underruns;
    return ps && ps->streaming;
  };
  int ua0 = -1, ub0 = -1, ua1 = -1, ub1 = -1;
  CHECK(streaming(a, 3, &ua0) && streaming(b, 3, &ub0));
  const double s0 = seconds_now();
  c.client.stop();
  const double stop_ms = 1000 * (seconds_now() - s0);
  const bool quiet = r.run_until([&] { return !streaming(a, 3, nullptr) && !streaming(b, 3, nullptr); }, 2.0);
  const double quiet_s = seconds_now() - s0;
  r.run(0.3);
  streaming(a, 3, &ua1);
  streaming(b, 3, &ub1);
  printf("  C's stop() took %.1f ms; A and B saw its stream stop after %.2f s, with %d and %d underruns\n",
         stop_ms, quiet_s, ua1 - ua0, ub1 - ub0);
  CHECK(quiet);
  CHECK(stop_ms < 300);
  CHECK_EQ(ua1, ua0);
  CHECK_EQ(ub1, ub0);
  for (player *p : { &a, &b, &c })
  {
    p->client.stop();
    p->stop_audio();
  }
  CHECK_EQ(r.server.bad.load(), 0);
  r.server.close();
}

static void test_room()
{
  printf("room\n");
  room r;
  CHECK(r.server.open());
  if (!r.server.port) return;

  // A sends bursts, its clock 100 ppm fast; B and C send tones; C runs at
  // 44.1 kHz, 100 ppm slow; D comes later
  player &a = r.add(1, 48000, 256, 0);
  player &b = r.add(2, 48000, 128, 500);
  player &c = r.add(3, 44100, 147, 700);
  player &d = r.add(4, 48000, 192, 900);
  a.bursts = true;
  a.ppm = 100;
  c.ppm = -100;

  a.client.setPeers(room::peers({ 2, 3 }));
  b.client.setPeers(room::peers({ 1, 3 }));
  c.client.setPeers(room::peers({ 1, 2 }));
  // (more than the default 20 ms: this machine may be busy with other things,
  // and the audio threads have no real-time priority)
  for (auto &p : r.players) p->client.setJitterMs(40);
  const double t0 = seconds_now();
  for (player *p : { &a, &b, &c })
  {
    std::string error;
    CHECK(p->client.start(p->cfg, &error));
    if (!error.empty()) printf("  start: %s\n", error.c_str());
    CHECK(p->client.running());
    p->run_audio();
  }

  // the first HELLO gets the cookie, the second (sent at once) binds
  CHECK(r.run_until([&] { return r.server.bound(1) && r.server.bound(2) && r.server.bound(3); }, 2.0));
  double at = 0;
  r.server.bound(1, &at);
  printf("  bound after %.1f ms; server RTT %.3f ms\n", 1000 * (at - t0), a.client.serverRttMs());
  CHECK(at - t0 < 0.5);
  CHECK(r.run_until([&] { return a.client.serverRttMs() > 0 && b.client.serverRttMs() > 0 && c.client.serverRttMs() > 0; }, 2.0));
  CHECK(a.client.serverRttMs() < 50);

  // everybody hears everybody
  const bool all = r.run_until([&] {
    return r.healthy(1, 2) && r.healthy(1, 3) && r.healthy(2, 1) && r.healthy(2, 3) && r.healthy(3, 1) && r.healthy(3, 2);
  }, 6.0);
  printf("  all healthy after %.2f s\n", seconds_now() - t0);
  CHECK(all);
  r.run(0.5);
  r.record(2.0);
  const double ab0 = median(burst_delays(a, r.heard(2, 1)));
  const double ac0 = median(burst_delays(a, r.heard(3, 1)));
  for (uint32_t l : { 1, 3 }) CHECK_TIMING(clean_share(r.heard(l, 2), b.tone_hz) > 0.8);
  for (uint32_t l : { 1, 2 }) CHECK_TIMING(clean_share(r.heard(l, 3), c.tone_hz) > 0.8);
  printf("  B hears C: %.0f%% clean; A hears B: %.0f%% clean\n", 100 * clean_share(r.heard(2, 3), c.tone_hz),
         100 * clean_share(r.heard(1, 2), b.tone_hz));
  const std::vector<PeerStats> bst = b.client.stats(), cst = c.client.stats();
  const PeerStats *ba = stats_of(bst, 1);
  const PeerStats *ca = stats_of(cst, 1);
  CHECK(ba && ca);
  if (ba && ca)
  {
    printf("  A -> B: %.2f ms, estimate %.2f (rtt %.2f, buffer %.1f, codec %.1f); A -> C: %.2f ms, estimate %.2f (rtt %.2f)\n",
           1000 * ab0, ba->oneWayMs, ba->rttMs, ba->bufferMs, ba->codecMs, 1000 * ac0, ca->oneWayMs, ca->rttMs);
    // (the estimate leaves out the time the codec takes, which in a debug
    // build with sanitizers is a good part of each block)
#if defined(NDEBUG) && !defined(LIVE_SANITIZED)
    const double tolerance = 5;
#else
    const double tolerance = 10;
#endif
    // A ping answered late, while this machine was busy, raises the smoothed
    // round trip for some seconds without changing the delay we hear: the
    // estimate may be over by up to half the round trip it reports.
    auto close_to = [&](double heard, const PeerStats *s) {
      const double over = s->oneWayMs - 1000 * heard;
      return over > -tolerance && over < tolerance + s->rttMs / 2;
    };
    CHECK_TIMING(close_to(ab0, ba));
    CHECK_TIMING(close_to(ac0, ca));
    CHECK(ba->rttMs > 0);
    CHECK_TIMING(ba->rttMs < 20);
    CHECK(ba->healthy && ba->streaming && ba->slot == 0);
  }
  for (player *p : { &a, &b, &c })
  {
    const Counters k = p->client.counters();
    CHECK(k.sent > 100 && k.received > 100 && k.accepted > 100);
    CHECK(k.helloAcks >= 2);
    CHECK_EQ(k.droppedBad, 0u);
    CHECK_EQ(k.droppedPeer, 0u);
    CHECK_EQ(k.droppedFilter, 0u);
    // (what came before the player's audio thread started)
    CHECK(k.droppedIdle < 50);
    if (p == &a) printf("  A: sent %llu, received %llu, dropped before its audio thread ran %llu, send thread raised: %s\n",
                        (unsigned long long)k.sent, (unsigned long long)k.received,
                        (unsigned long long)k.droppedIdle, k.sendThreadRealtime ? "yes" : "no (not allowed)");
  }

  // the clocks drift (for minutes in live_unit_test): the delay stays
  int dropouts0 = 0;
  for (const PeerStats *s : { ba, ca }) if (s) dropouts0 += s->underruns + s->overruns + s->dropped;
  r.run(3.0);
  r.record(2.0);
  const double ab1 = median(burst_delays(a, r.heard(2, 1)));
  const double ac1 = median(burst_delays(a, r.heard(3, 1)));
  const std::vector<PeerStats> bst1 = b.client.stats(), cst1 = c.client.stats();
  int dropouts = -dropouts0;
  for (const PeerStats *s : { stats_of(bst1, 1), stats_of(cst1, 1) }) if (s) dropouts += s->underruns + s->overruns + s->dropped;
  printf("  %.1f s later: A -> B %.2f ms (was %.2f), A -> C %.2f ms (was %.2f), %d dropouts\n", 5.0,
         1000 * ab1, 1000 * ab0, 1000 * ac1, 1000 * ac0, dropouts);
  // AOO's clocks are the times processBlock() is called at: a sender whose
  // calls come late by d on average claims a slower clock until its DLL has
  // moved from its first call's lateness to d, over a minute, and its
  // listeners' delay moves by that much meanwhile (a listener's own
  // lateness moves it the other way; README.md, "AOO's side"). An audio
  // interface calls on time; macOS CI VMs wake these threads up to 80 ms
  // late, and they catch up.
  printf("  audio threads late (first, mean, max): A %.2f, %.2f, %.1f ms; B %.2f, %.2f, %.1f; C %.2f, %.2f, %.1f\n",
         1000 * a.late_first, 1000 * a.late_mean, 1000 * a.late_max, 1000 * b.late_first, 1000 * b.late_mean,
         1000 * b.late_max, 1000 * c.late_first, 1000 * c.late_mean, 1000 * c.late_max);
  auto settling = [](const player &p) { return fabs(p.late_mean - p.late_first); };
  CHECK_TIMING(dropouts == 0);
  CHECK_TIMING(fabs(ab1 - ab0) < 0.0015 + settling(a) + settling(b));
  CHECK_TIMING(fabs(ac1 - ac0) < 0.0015 + settling(a) + settling(c));

  // 5% of the copies to C lost: resent
  r.server.with(3, [](fake_server::peer &p) { p.loss = 0.05; });
  r.run(3.0);
  r.server.with(3, [](fake_server::peer &p) { p.loss = 0; });
  const std::vector<PeerStats> cst2 = c.client.stats();
  const PeerStats *cb = stats_of(cst2, 2);
  CHECK(cb != nullptr);
  if (cb)
  {
    printf("  C from B, 5%% lost: resent %d, dropped %d, loss %.1f%%, healthy %d\n", cb->resent, cb->dropped,
           cb->lossPct, (int)cb->healthy);
    CHECK(cb->resent > 10);
    CHECK_TIMING(cb->dropped <= cb->resent / 2);
    CHECK(cb->lossPct > 0.3); // (since the stream started, 3 of about 11 s at 5%)
  }
  CHECK(r.run_until([&] { return r.healthy(3, 2); }, 3.0));

  // a forged HELLO_ACK and garbage, sent to A's address: ignored
  {
    sockaddr_in ep;
    r.server.with(1, [&](fake_server::peer &p) { ep = p.ep; });
    uint8_t ack[wire::kHelloAckSize];
    wire::writeHelloAck(ack, 12345, 999);
    r.server.send_to(ep, ack, sizeof(ack));
    const uint8_t junk[5] = { 'X', 'L', 1, 2, 0 };
    r.server.send_to(ep, junk, sizeof(junk));
    const long unverified = r.server.unverified.load();
    r.run(2.5); // a HELLO from everyone, with the right cookie still
    CHECK_EQ(a.client.counters().droppedBad, 2u);
    CHECK_EQ(r.server.unverified.load(), unverified);
  }

  // the room's flag off for A: its slots go quiet at once, its stream stops
  a.cfg.roomOn = false;
  a.client.update(a.cfg);
  CHECK(!a.client.slotHealthy(0) && !a.client.slotHealthy(1));
  CHECK(r.run_until([&] { return !r.healthy(2, 1) && !r.healthy(3, 1); }, 6.0));
  a.cfg.roomOn = true;
  a.client.update(a.cfg);
  CHECK(r.run_until([&] { return r.healthy(1, 2) && r.healthy(2, 1) && r.healthy(3, 1); }, 6.0));

  // B leaves, D comes: D takes B's slot with A and C
  b.client.stop();
  CHECK(!b.client.running());
  b.stop_audio();
  r.server.with(2, [](fake_server::peer &p) { p.in_room = false; });
  const int slot = a.client.slotOf(2);
  a.client.setPeers(room::peers({ 3, 4 }));
  c.client.setPeers(room::peers({ 1, 4 }));
  d.client.setPeers(room::peers({ 1, 3 }));
  CHECK_EQ(a.client.slotOf(2), -1);
  CHECK_EQ(a.client.slotOf(4), slot);
  CHECK(!a.client.slotHealthy(slot));
  std::string error;
  CHECK(d.client.start(d.cfg, &error));
  d.run_audio();
  CHECK(r.run_until([&] { return r.healthy(1, 4) && r.healthy(3, 4) && r.healthy(4, 1) && r.healthy(4, 3); }, 6.0));
  r.run(0.3);
  r.record(1.5);
  const recording ad = r.heard(1, 4);
  const double clean = clean_share(ad, d.tone_hz);
  const double snr_b = tone_snr(ad.samples.data(), ad.samples.size(), b.tone_hz, ad.rate);
  printf("  A's slot %d: D %.0f%% clean; B's tone at %.1f dB\n", slot, 100 * clean, snr_b);
  CHECK_TIMING(clean > 0.8);
  CHECK(snr_b < -20);
  const std::vector<PeerStats> ast = a.client.stats();
  const PeerStats *ad_st = stats_of(ast, 4);
  CHECK(ad_st && ad_st->slot == slot && ad_st->healthy);
  CHECK(stats_of(ast, 2) == nullptr);

  // stop: quickly, and silent afterwards
  for (player *p : { &a, &c, &d })
  {
    const double s = seconds_now();
    p->client.stop();
    CHECK(seconds_now() - s < 1.0);
    CHECK(!p->client.running());
    p->stop_audio();
    CHECK(!p->client.slotHealthy(0));
    CHECK_EQ(p->client.slotOf(1), -1);
  }
  printf("  server: %ld HELLOs (%ld verified, %ld not), %ld DATA in, %ld out, %ld lost, %ld bad\n",
         r.server.hellos.load(), r.server.verified.load(), r.server.unverified.load(), r.server.data_in.load(),
         r.server.data_out.load(), r.server.lost.load(), r.server.bad.load());
  CHECK_EQ(r.server.bad.load(), 0);
  r.server.close();
}

int main()
{
  printf("live_client_test\n");
#ifdef _WIN32
  WSADATA wd;
  WSAStartup(MAKEWORD(2, 2), &wd);
  timeBeginPeriod(1); // the audio threads' sleeps
#endif
  test_start_errors();
  test_restart();
  test_room();
#ifdef _WIN32
  timeEndPeriod(1);
  WSACleanup();
#endif
  return test_summary("live_client_test");
}
