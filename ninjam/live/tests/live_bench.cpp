/*
    Jamsa Live - live_bench.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Benchmarks of Live mode. Not a test: ctest doesn't run it, and nothing in
  it passes or fails. Each mode writes CSV (a header line, then a line for
  each measurement) to --out FILE (or stdout), and what it is doing to
  stderr.

  live_bench net [--seconds 60] [--seeds 3] [--buffers 10,20,30,40,60,80]
                 [--jitter 0,2,5,10,20] [--loss 0,1,3,5,10,burst30@0.2,burst100@0.1]
                 [--delay 5] [--block 128] [--rooms 2]
    A bad connection, reproducibly: senders (room - 1 of them) and a
    listener (each a Core, as in the plug-in) on live_sim.h's virtual
    network, through its server. The
    listener's connection (both ways: the audio, and its resend requests)
    loses datagrams at random (loss in percent) or in bursts (burstMS@PER_S:
    bursts of MS ms, PER_S of them a second, on the audio's way), and delays
    each by up to the jitter more, uniformly (which reorders them); each of
    the four links also takes --delay ms (so a round trip is 4 x delay).
    Each sender plays 10 ms bursts of a tone every 200 ms; for each burst the
    listener heard, the delay (the centre of its energy). Per run: dropouts
    (underruns, overruns and blocks lost for good, as the plug-in's /live
    counts them; summed over the streams, a minute per stream, and the
    worst stream's), blocks resent and blocks Opus concealed, the share of
    the time the listener's slots were healthy (the plug-in plays a player
    live only then; per stream, the worst, and all at once), the delay
    heard and the statistics' estimate. The senders don't hear each other.

  live_bench bw [--seconds 20] [--kbps 64,128,256,510] [--frames 120,240,480]
                [--rooms 2,3,4,5,6,7,8] [--signals music,noise]
    Bandwidth per player: rooms where everybody plays (Cores on the virtual
    network, no loss), each datagram counted where the server gets it and
    where it sends it. Up and down: datagrams/s, kbit/s of UDP payload, and
    on the wire with IPv4 and UDP headers (28 bytes a datagram); the HELLOs
    (one each way every 2 s, 40 and 24 bytes) are added. The sizes of the
    stream's data packets, for the server benchmark.

  live_bench cpu [--listeners 1,3,5,7] [--blocks 64,128,256,512] [--seconds 20]
                 [--reps 3] [--rate 48000] [--kbps 128] [--frame 240] [--samples FILE]
    The audio thread's cost: Client::processBlock() per host block, with N
    others in the room (N streams to decode, ours to N listeners), on real
    sockets. This process holds one Client with an audio thread paced by
    the clock (SCHED_FIFO when the system allows, as a DAW's); a child
    process holds a minimal server on the loopback interface (HELLO /
    HELLO_ACK with a cookie, DATA copied to the room) and the N other
    players, each a Client with an audio thread of its own. Per block: the
    wall time and the thread's CPU time inside processBlock(). Per run: the
    receive and send threads' CPU (/proc/self/task/<tid>/schedstat), and
    what our player sent and got through the server (datagrams, bytes).

  live_bench rss [--listeners 7] [--seconds 120] [--block 256] [--loss 0]
    Memory: as cpu, one run, the RSS of this process (one Client, N others
    playing to it) every second.

  live_bench relay --port P [--seconds 20] [--warm 3] [--aoo-bytes 105]
                   [--data-rate 200] [--uni-bytes 45] [--uni-rate 4] [--uni-near 8]
                   [--uni-far-rate 1] [--hello 2] [--label TEXT] [--hist FILE]
    The UDP side of ninjam/server/tests/bench_live.py: players that the
    script logged in (it keeps their TCP connections alive) send what a
    Live player sends to a real server on 127.0.0.1:P: a HELLO every 2 s
    (with the cookie), --data-rate DATA datagrams a second to the room with
    --aoo-bytes of payload, and --uni-rate a second to each of the first
    --uni-near other players (those a player listens to) and --uni-far-rate
    to each of the rest, with --uni-bytes (AOO's pings and the like; the
    defaults are what bw measures at LiveKbps 128 and LiveFrame 240, with
    music: 4 a second to each other player in a room of up to 8, and about
    1 to each player past the 8 a player listens to). Reads "id key room" lines
    on stdin. Each copy that comes back carries its send time; the kernel's
    receive time stamp (SO_TIMESTAMPNS) gives how long the server took
    (loopback both ways included). One CSV line: copies expected, got,
    lost, latency percentiles. Says "measure start" and "measure end" on
    stderr when the measured seconds begin and end.

  Linux (and macOS, without the per-thread CPU and the relay mode). POSIX
  only: not built on Windows.

*/

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/net_tstamp.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#include "liveclient.h"
#include "livecore.h"
#include "live_sim.h"
#include "benchutil.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using jamsa_live::Client;
using jamsa_live::Config;
using jamsa_live::Core;
using jamsa_live::Peer;
using jamsa_live::PeerStats;
namespace wire = jamsa_live::wire;

using namespace bench;

// ---------------------------------------------------------------------------
// net: a bad connection, on the virtual network
// ---------------------------------------------------------------------------

using live_sim::net;
using live_sim::player;

// Hann-shaped 10 ms bursts of 1 kHz every 200 ms (as live_unit_test's)
struct Bursts
{
  double period = 0.2, len = 0.01, hz = 1000;

  void install(player &p) const
  {
    const Bursts b = *this;
    player *pp = &p;
    p.input = [b, pp](int64_t index, float *l, float *r, int n) {
      const int64_t P = (int64_t)llround(b.period * pp->rate), L = (int64_t)llround(b.len * pp->rate);
      for (int i = 0; i < n; i++)
      {
        const int64_t k = (index + i) % P;
        double v = 0;
        if (k < L)
          v = 0.5 * (0.5 - 0.5 * cos(6.283185307179586 * (double)k / (double)L))
            * sin(6.283185307179586 * b.hz * (double)k / pp->rate);
        l[i] = (float)v;
        r[i] = (float)v;
      }
    };
  }

  // The bursts that q recorded on its slot for p (since its recording was
  // cleared): each one's delay (the centre of its energy, in true time,
  // against the centre of the burst p played). Not the first and last burst
  // of the recording (they may be cut off). *played: the bursts looked at.
  std::vector<double> delays(const player &p, const player &q, int slot, int *played) const
  {
    std::vector<double> out;
    *played = 0;
    if (slot < 0 || q.rec_time.empty()) return out;
    const std::vector<float> &r = q.rec[slot];
    const int64_t P = (int64_t)llround(period * p.rate), L = (int64_t)llround(len * p.rate);
    const double first = p.sample_time(L / 2);
    const double step = (double)P / p.eff_rate();
    long m0 = -1, m1 = -1;
    std::vector<double> e, et;
    for (size_t i = 0; i < r.size(); i++)
    {
      const double t = q.rec_sample_time(i);
      const long m = (long)floor((t - first) / step);
      if (m < 0) continue;
      if (m0 < 0) m0 = m;
      m1 = std::max(m1, m);
      const size_t k = (size_t)(m - m0);
      if (k >= e.size()) { e.resize(k + 1, 0); et.resize(k + 1, 0); }
      const double x2 = (double)r[i] * r[i];
      e[k] += x2;
      et[k] += x2 * (t - (first + m * step));
    }
    // a burst as loud as half the loudest of a clean one (0.5 peak: 10 ms of Hann^2 at 48 kHz)
    double full = 0;
    for (int64_t i = 0; i < L; i++)
    {
      const double w = 0.5 * (0.5 - 0.5 * cos(6.283185307179586 * (double)i / (double)L));
      full += w * w * 0.5;
    }
    full *= q.rate / p.rate;
    for (size_t k = 1; k + 1 < e.size(); k++)
    {
      (*played)++;
      if (e[k] > 0.25 * full) out.push_back(et[k] / e[k]);
    }
    return out;
  }
};

static const PeerStats *stats_of(const std::vector<PeerStats> &all, uint32_t id)
{
  for (const PeerStats &s : all) if (s.id == id) return &s;
  return nullptr;
}

struct LossKind
{
  std::string name;
  double pct = 0;       // random loss, each datagram
  double burstMs = 0;   // bursts of this length
  double perSecond = 0; // this many a second
};

static LossKind parse_loss(const std::string &s)
{
  LossKind k;
  k.name = s;
  if (!s.compare(0, 5, "burst"))
  {
    k.burstMs = atof(s.c_str() + 5);
    const size_t at = s.find('@');
    k.perSecond = at == std::string::npos ? 0.2 : atof(s.c_str() + at + 1);
  }
  else k.pct = atof(s.c_str());
  return k;
}

static int mode_net(const Args &a)
{
  const double seconds = a.num("seconds", 60), warm = 2.0;
  const int seeds = (int)a.num("seeds", 3);
  const std::vector<double> buffers = a.nums("buffers", "10,20,30,40,60,80");
  const std::vector<double> jitters = a.nums("jitter", "0,2,5,10,20");
  const std::vector<std::string> losses = a.list("loss", "0,1,3,5,10,burst30@0.2,burst100@0.1");
  const double delayMs = a.num("delay", 5);
  const int block = (int)a.num("block", 128);
  const std::vector<double> rooms = a.nums("rooms", "2");
  Csv out(a.str("out", "-"));
  out.line("loss,loss_pct,burst_ms,bursts_per_s,jitter_ms,link_delay_ms,buffer_ms,block,seed,seconds,"
           "datagrams_lost,underruns,overruns,concealed,resent,dropouts,dropouts_per_min,healthy_pct,"
           "delay_med_ms,delay_p95_ms,delay_max_ms,bursts_heard,bursts_played,estimate_ms,buffer_stat_ms,"
           "loss_stat_pct,wall_s,room,streams,dropouts_per_min_worst,healthy_pct_worst,all_healthy_pct");
  for (double roomD : rooms)
  for (const std::string &ls : losses)
  {
    const LossKind lk = parse_loss(ls);
    const int S = std::max(1, (int)roomD - 1); // senders; the listener is the last player
    for (double jitter : jitters)
      for (double buffer : buffers)
        for (int seed = 1; seed <= seeds; seed++)
        {
          const double w0 = mono_seconds();
          net n((unsigned)seed);
          std::vector<player *> snds;
          for (int i = 0; i < S; i++) snds.push_back(&n.add((uint32_t)(i + 1), 48000, block));
          player &lis = n.add((uint32_t)(S + 1), 48000, block);
          const uint32_t lisId = (uint32_t)(S + 1);
          for (player *p : snds) p->up.delay = p->down.delay = delayMs / 1000.0;
          lis.up.delay = lis.down.delay = delayMs / 1000.0;
          // the senders don't hear each other (it would only slow the simulation): only what
          // the listener sends them (its resend requests, pings) reaches them
          n.copy_filter = [lisId](uint32_t from, uint32_t to, const uint8_t *, size_t) { return to == lisId || from == lisId; };
          // the listener's connection: the audio, and its requests for resends
          lis.down.loss = lis.up.loss = lk.pct / 100.0;
          lis.down.jitter = lis.up.jitter = jitter / 1000.0;
          if (lk.burstMs > 0)
          {
            // (the chance per datagram: about 200 a second come down from each sender at 240-sample frames)
            lis.down.burst = lk.perSecond / (200.0 * S);
            lis.down.burst_len = lk.burstMs / 1000.0;
          }
          Bursts bu;
          for (player *p : snds) bu.install(*p);
          lis.send = false;
          n.start_all();
          lis.core->setJitterMs((int)buffer);
          n.run(warm);
          std::vector<int> slot(S);
          std::vector<PeerStats> s0(S);
          const std::vector<PeerStats> all0 = lis.core->stats(n.now);
          for (int i = 0; i < S; i++)
          {
            slot[i] = lis.core->slotOf((uint32_t)(i + 1));
            const PeerStats *st = stats_of(all0, (uint32_t)(i + 1));
            if (st) s0[i] = *st;
          }
          const long lost0 = lis.down.lost + lis.up.lost;
          std::vector<double> d;
          int played = 0;
          long looks = 0, allHealthy = 0;
          std::vector<long> healthy(S, 0);
          double done = 0;
          while (done < seconds - 1e-9)
          {
            const double chunk = std::min(4.0, seconds - done);
            lis.clear_recording();
            lis.recording = true;
            for (double t = 0; t < chunk - 1e-9; t += 0.25)
            {
              n.run(std::min(0.25, chunk - t));
              looks++;
              bool all = true;
              for (int i = 0; i < S; i++)
              {
                if (lis.core->slotHealthy(slot[i])) healthy[i]++;
                else all = false;
              }
              if (all) allHealthy++;
            }
            lis.recording = false;
            for (int i = 0; i < S; i++)
            {
              int pl = 0;
              for (double x : bu.delays(*snds[i], lis, slot[i], &pl)) d.push_back(1000.0 * x);
              played += pl;
            }
            done += chunk;
          }
          lis.clear_recording();
          // summed over the streams (the listener hears S), per minute per stream, and the worst stream
          int under = 0, over = 0, dropped = 0, resent = 0, worst = 0;
          double estimate = 0, bufferStat = 0, lossStat = 0, healthyWorst = 100;
          const std::vector<PeerStats> all1 = lis.core->stats(n.now);
          for (int i = 0; i < S; i++)
          {
            const PeerStats *st = stats_of(all1, (uint32_t)(i + 1));
            if (!st) continue;
            const int u = st->underruns - s0[i].underruns, o = st->overruns - s0[i].overruns;
            const int dr = st->dropped - s0[i].dropped;
            under += u;
            over += o;
            dropped += dr;
            resent += st->resent - s0[i].resent;
            worst = std::max(worst, u + o + dr);
            // the statistics' estimate (AOO's pings measure the round trip on the wall clock, which
            // says nothing here: the network's known delay instead, with the jitter's average)
            estimate += 2 * delayMs + jitter / 2 + st->oneWayMs - st->rttMs / 2;
            bufferStat += st->bufferMs;
            lossStat += st->lossPct;
            healthyWorst = std::min(healthyWorst, looks ? 100.0 * healthy[i] / looks : 0.0);
          }
          estimate /= S;
          bufferStat /= S;
          lossStat /= S;
          long healthySum = 0;
          for (long h : healthy) healthySum += h;
          const int dropouts = under + over + dropped;
          const double healthyPct = looks ? 100.0 * (double)healthySum / ((double)looks * S) : 0.0;
          out.line("%s,%.1f,%.0f,%.2f,%.0f,%.1f,%.0f,%d,%d,%.0f,%ld,%d,%d,%d,%d,%d,%.2f,%.1f,%.2f,%.2f,%.2f,%zu,%d,%.2f,%.2f,"
                   "%.2f,%.2f,%d,%d,%.2f,%.1f,%.1f",
                   lk.name.c_str(), lk.pct, lk.burstMs, lk.perSecond, jitter, delayMs, buffer, block, seed, seconds,
                   lis.down.lost + lis.up.lost - lost0, under, over, dropped, resent, dropouts,
                   dropouts * 60.0 / seconds / S, healthyPct, pct(d, 50), pct(d, 95),
                   pct(d, 100), d.size(), played, estimate, bufferStat, lossStat, mono_seconds() - w0,
                   S + 1, S, worst * 60.0 / seconds, healthyWorst, looks ? 100.0 * allHealthy / looks : 0.0);
          say("net room %d %s jitter %.0f buffer %.0f seed %d: %d dropouts (%d under, %d over, %d concealed), %d resent, "
              "healthy %.0f%% (all %d at once %.0f%%), delay %.1f ms (%.1f s)\n",
              S + 1, lk.name.c_str(), jitter, buffer, seed, dropouts, under, over, dropped, resent, healthyPct, S,
              looks ? 100.0 * allHealthy / looks : 0.0, pct(d, 50), mono_seconds() - w0);
        }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// bw: bandwidth per player, on the virtual network
// ---------------------------------------------------------------------------

static int mode_bw(const Args &a)
{
  const double seconds = a.num("seconds", 20), warm = 3.0;
  const std::vector<double> kbpsList = a.nums("kbps", "64,128,256,510");
  const std::vector<double> frames = a.nums("frames", "120,240,480");
  const std::vector<double> rooms = a.nums("rooms", "2,3,4,5,6,7,8");
  const std::vector<std::string> signals = a.list("signals", "music,noise");
  const double hello = 0.5; // HELLOs a second, each way (one every 2 s)
  Csv out(a.str("out", "-"));
  out.line("signal,kbps,frame,room,seconds,up_dgram_s,up_data_dgram_s,up_other_dgram_s,up_payload_kbps,up_wire_kbps,"
           "down_dgram_s,down_payload_kbps,down_wire_kbps,data_aoo_bytes_mean,data_aoo_bytes_p50,data_aoo_bytes_p99,"
           "other_aoo_bytes_mean,other_dgram_s_per_peer,up_dgram_s_min,up_dgram_s_max,down_dgram_s_min,down_dgram_s_max");
  for (const std::string &sig : signals)
    for (double kbps : kbpsList)
      for (double frame : frames)
        for (double room : rooms)
        {
          net n(7);
          const int R = (int)room;
          std::vector<Signal> sigs(R);
          for (int i = 0; i < R; i++)
          {
            player &p = n.add((uint32_t)(i + 1), 48000, 128);
            p.core->configure((int)kbps, (int)frame);
            sigs[i].init(sig == "noise", 48000, i);
            const Signal *s = &sigs[i];
            p.input = [s](int64_t index, float *l, float *r, int k) { s->fill(index, l, r, k); };
          }
          bool counting = false;
          std::vector<double> upD(R + 1, 0), upB(R + 1, 0), upData(R + 1, 0), upOther(R + 1, 0), upOtherB(R + 1, 0);
          std::vector<double> downD(R + 1, 0), downB(R + 1, 0);
          std::vector<double> dataSizes;
          n.server_filter = [&](const wire::FromClient &m) {
            if (!counting || m.sender > (uint32_t)R) return true;
            upD[m.sender]++;
            upB[m.sender] += (double)(wire::kClientHeaderSize + m.size);
            if (m.dest == 0)
            {
              upData[m.sender]++;
              if (m.sender == 1) dataSizes.push_back((double)m.size);
            }
            else
            {
              upOther[m.sender]++;
              upOtherB[m.sender] += (double)m.size;
            }
            return true;
          };
          n.copy_filter = [&](uint32_t, uint32_t to, const uint8_t *, size_t size) {
            if (counting && to <= (uint32_t)R)
            {
              downD[to]++;
              downB[to] += (double)(wire::kServerHeaderSize + size);
            }
            return true;
          };
          n.start_all();
          n.run(warm);
          counting = true;
          n.run(seconds);
          counting = false;
          // per player, then the average over the players
          std::vector<double> upDs, downDs;
          double uD = 0, uData = 0, uOther = 0, uB = 0, dD = 0, dB = 0, oB = 0;
          for (int i = 1; i <= R; i++)
          {
            upDs.push_back(upD[i] / seconds + hello);
            downDs.push_back(downD[i] / seconds + hello);
            uD += upD[i] / seconds + hello;
            uData += upData[i] / seconds;
            uOther += upOther[i] / seconds;
            uB += upB[i] / seconds + hello * wire::kHelloSize;
            dD += downD[i] / seconds + hello;
            dB += downB[i] / seconds + hello * wire::kHelloAckSize;
            oB += upOther[i] > 0 ? upOtherB[i] / upOther[i] : 0;
          }
          uD /= R; uData /= R; uOther /= R; uB /= R; dD /= R; dB /= R; oB /= R;
          const double kbit = 8.0 / 1000.0;
          out.line("%s,%.0f,%.0f,%d,%.0f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.0f,%.0f,%.1f,%.2f,%.1f,%.1f,%.1f,%.1f",
                   sig.c_str(), kbps, frame, R, seconds, uD, uData, uOther, uB * kbit, (uB + 28 * uD) * kbit, dD, dB * kbit,
                   (dB + 28 * dD) * kbit, mean(dataSizes), pct(dataSizes, 50), pct(dataSizes, 99), oB,
                   R > 1 ? uOther / (R - 1) : 0.0, pct(upDs, 0), pct(upDs, 100), pct(downDs, 0), pct(downDs, 100));
          say("bw %s %.0f kbps, %.0f samples, room of %d: up %.0f/s %.1f kbit/s (wire %.1f), down %.0f/s %.1f kbit/s "
              "(wire %.1f), data packets %.0f bytes\n",
              sig.c_str(), kbps, frame, R, uD, uB * kbit, (uB + 28 * uD) * kbit, dD, dB * kbit, (dB + 28 * dD) * kbit,
              mean(dataSizes));
        }
  return 0;
}

// ---------------------------------------------------------------------------
// cpu, rss: real Clients on the loopback interface
// ---------------------------------------------------------------------------

static uint64_t key_of(uint32_t id)
{
  return (0x9E3779B97F4A7C15ull * (id + 17)) ^ 0x5bd1e9955bd1e995ull;
}

// What ninjam/server does for Live, minus the TCP side and the rate limits:
// one room, HELLO / HELLO_ACK with a cookie, DATA copied to the room with
// its header rewritten in place. Counts each player's datagrams.
struct Forwarder
{
  struct Peer
  {
    uint32_t id = 0;
    uint64_t key = 0;
    bool bound = false;
    sockaddr_in ep;
    // since mark(): up = from it, down = to it (UDP payload bytes)
    uint64_t upD = 0, upB = 0, upHello = 0, upData = 0, upOther = 0, downD = 0, downB = 0;
  };

  int sock = -1;
  uint16_t port = 0;
  double loss = 0;
  std::mt19937 rng{11};
  std::thread thread;
  std::atomic<bool> quit{false};
  std::mutex mutex;
  std::vector<Peer> peers;

  bool open()
  {
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return false;
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, (const sockaddr *)&a, sizeof(a)) != 0) return false;
    socklen_t len = sizeof(a);
    getsockname(sock, (sockaddr *)&a, &len);
    port = ntohs(a.sin_port);
    timeval tv = { 0, 50000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int bufsize = 1 << 20;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
    thread = std::thread([this] { loop(); });
    return true;
  }

  void close()
  {
    quit.store(true);
    if (thread.joinable()) thread.join();
    if (sock >= 0) ::close(sock);
    sock = -1;
  }

  void add(uint32_t id)
  {
    std::lock_guard<std::mutex> lock(mutex);
    Peer p;
    p.id = id;
    p.key = key_of(id);
    memset(&p.ep, 0, sizeof(p.ep));
    peers.push_back(p);
  }

  void mark()
  {
    std::lock_guard<std::mutex> lock(mutex);
    for (Peer &p : peers) p.upD = p.upB = p.upHello = p.upData = p.upOther = p.downD = p.downB = 0;
  }

  // "id upD upB upHello upData upOther downD downB;..." for each player
  std::string report()
  {
    std::lock_guard<std::mutex> lock(mutex);
    std::string s;
    char buf[256];
    for (const Peer &p : peers)
    {
      snprintf(buf, sizeof(buf), "%u %llu %llu %llu %llu %llu %llu %llu;", p.id, (unsigned long long)p.upD,
               (unsigned long long)p.upB, (unsigned long long)p.upHello, (unsigned long long)p.upData,
               (unsigned long long)p.upOther, (unsigned long long)p.downD, (unsigned long long)p.downB);
      s += buf;
    }
    return s;
  }

  static uint64_t cookie_for(const Peer &p, const sockaddr_in &from)
  {
    uint64_t x = 0x243f6a8885a308d3ull ^ ((uint64_t)p.id << 32) ^ p.key ^ ((uint64_t)from.sin_addr.s_addr << 16) ^ from.sin_port;
    for (int i = 0; i < 3; i++)
    {
      x ^= x >> 31;
      x *= 0x9E3779B97F4A7C15ull;
    }
    return x | 1;
  }

  void send_to(Peer &q, const uint8_t *data, size_t size)
  {
    if (sendto(sock, data, size, 0, (const sockaddr *)&q.ep, sizeof(q.ep)) == (ssize_t)size)
    {
      q.downD++;
      q.downB += size;
    }
  }

  void loop()
  {
    uint8_t buf[wire::kMaxDatagram + 1];
    while (!quit.load())
    {
      sockaddr_in from;
      socklen_t len = sizeof(from);
      const int n = (int)recvfrom(sock, buf, sizeof(buf), 0, (sockaddr *)&from, &len);
      if (n <= 0) continue;
      wire::FromClient m;
      if (n > (int)wire::kMaxDatagram || !wire::parseFromClient(buf, (size_t)n, m)) continue;
      std::lock_guard<std::mutex> lock(mutex);
      Peer *s = nullptr;
      for (Peer &p : peers) if (p.id == m.sender) s = &p;
      if (!s || s->key != m.key) continue;
      s->upD++;
      s->upB += (uint64_t)n;
      if (m.type == wire::kHello)
      {
        s->upHello++;
        const uint64_t c = cookie_for(*s, from);
        if (m.cookie == c)
        {
          s->bound = true;
          s->ep = from;
        }
        uint8_t ack[wire::kHelloAckSize];
        const size_t k = wire::writeHelloAck(ack, m.stamp, c);
        if (sendto(sock, ack, k, 0, (const sockaddr *)&from, sizeof(from)) == (ssize_t)k && s->bound
            && !memcmp(&from, &s->ep, sizeof(from)))
        {
          s->downD++;
          s->downB += k;
        }
        continue;
      }
      if (m.dest == 0) s->upData++;
      else s->upOther++;
      wire::writeServerHeader(buf + 12, wire::kData, m.sender);
      for (Peer &q : peers)
      {
        if (q.id == m.sender || !q.bound || (m.dest && q.id != m.dest)) continue;
        if (loss > 0 && std::uniform_real_distribution<double>(0, 1)(rng) < loss) continue;
        send_to(q, buf + 12, (size_t)n - 12);
      }
    }
  }
};

// A player's audio thread, paced by the clock as an audio interface calls
// it; the measured one records each block's cost.
struct AudioThread
{
  Client *client = nullptr;
  double rate = 48000;
  int block = 256;
  Signal signal;
  std::thread thread;
  std::atomic<bool> quit{false};
  std::atomic<bool> realtime{false};
  std::atomic<long> tid{0};
  std::atomic<bool> ready{false};
  // measuring (the main thread turns 'recording' on and off)
  std::atomic<bool> recording{false};
  std::atomic<size_t> count{0};
  std::vector<float> wallUs, cpuUs, lateUs;

  void start(Client *c, double r, int b, int seed, double seconds)
  {
    client = c;
    rate = r;
    block = b;
    signal.init(false, r, seed);
    const size_t cap = (size_t)(seconds * r / b) + 1000;
    wallUs.assign(cap, 0.0f);
    cpuUs.assign(cap, 0.0f);
    lateUs.assign(cap, 0.0f);
    thread = std::thread([this] { run(); });
    while (!ready.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  void stop()
  {
    quit.store(true);
    if (thread.joinable()) thread.join();
  }

  void run()
  {
    tid.store(this_tid());
    realtime.store(make_realtime(70));
    std::vector<float> l(block), r(block);
    client->prepare(rate, block);
    ready.store(true);
    const double start = mono_seconds() + 0.005;
    for (uint64_t n = 0; !quit.load(std::memory_order_relaxed); n++)
    {
      // when the interface wants this block; a late one runs at once (and the next on time)
      const double due = start + (double)n * block / rate;
      sleep_until(due);
      const double t0 = mono_seconds();
      signal.fill((int64_t)n * block, l.data(), r.data(), block);
      const double c0 = thread_cpu_seconds(), w0 = mono_seconds();
      client->processBlock(l.data(), r.data(), block, true);
      const double w1 = mono_seconds(), c1 = thread_cpu_seconds();
      if (recording.load(std::memory_order_relaxed))
      {
        const size_t k = count.load(std::memory_order_relaxed);
        if (k < wallUs.size())
        {
          wallUs[k] = (float)((w1 - w0) * 1e6);
          cpuUs[k] = (float)((c1 - c0) * 1e6);
          lateUs[k] = (float)((t0 - due) * 1e6);
          count.store(k + 1, std::memory_order_release);
        }
      }
    }
  }
};

static Config config_for(uint32_t id, uint16_t port, int kbps, int frame)
{
  Config c;
  c.myId = id;
  c.key = key_of(id);
  c.serverIp = htonl(INADDR_LOOPBACK);
  c.udpPort = port;
  c.kbps = kbps;
  c.frameSamples = frame;
  c.udpOpen = c.roomOn = c.maySend = true;
  return c;
}

static std::vector<Peer> peers_except(uint32_t self, int players)
{
  std::vector<Peer> v;
  for (int i = 1; i <= players; i++)
  {
    if ((uint32_t)i == self) continue;
    Peer p;
    p.id = (uint32_t)i;
    p.name = "p" + std::to_string(i);
    p.bound = true;
    v.push_back(p);
  }
  return v;
}

static bool read_line(int fd, std::string *line, double timeout)
{
  line->clear();
  const double end = mono_seconds() + timeout;
  for (;;)
  {
    const double left = end - mono_seconds();
    if (left <= 0) return false;
    pollfd p = { fd, POLLIN, 0 };
    if (poll(&p, 1, (int)ceil(left * 1000)) <= 0) continue;
    char c;
    const ssize_t n = read(fd, &c, 1);
    if (n <= 0) return false;
    if (c == '\n') return true;
    *line += c;
  }
}

static void write_line(int fd, const std::string &s)
{
  const std::string l = s + "\n";
  if (write(fd, l.data(), l.size()) != (ssize_t)l.size()) {}
}

// The child: the server and players 2..others+1, until told to quit.
static void run_room(int others, double rate, int block, int kbps, int frame, double loss, int cmdFd, int replyFd)
{
  Forwarder fwd;
  fwd.loss = loss;
  const int players = others + 1;
  for (int i = 1; i <= players; i++) fwd.add((uint32_t)i);
  if (!fwd.open())
  {
    write_line(replyFd, "error");
    _exit(1);
  }
  std::vector<std::unique_ptr<Client>> clients;
  std::vector<std::unique_ptr<AudioThread>> audio;
  for (int i = 2; i <= players; i++)
  {
    std::unique_ptr<Client> c(new Client);
    std::string err;
    if (!c->start(config_for((uint32_t)i, fwd.port, kbps, frame), &err))
    {
      write_line(replyFd, "error " + err);
      _exit(1);
    }
    c->setPeers(peers_except((uint32_t)i, players));
    std::unique_ptr<AudioThread> t(new AudioThread);
    t->start(c.get(), rate, block, i, 0);
    clients.push_back(std::move(c));
    audio.push_back(std::move(t));
  }
  write_line(replyFd, "port " + std::to_string(fwd.port));
  double nextPoll = 0;
  for (;;)
  {
    pollfd p = { cmdFd, POLLIN, 0 };
    if (poll(&p, 1, 10) > 0)
    {
      std::string cmd;
      if (!read_line(cmdFd, &cmd, 1.0) || cmd == "quit") break;
      if (cmd == "mark")
      {
        fwd.mark();
        write_line(replyFd, "ok");
      }
      else if (cmd == "report") write_line(replyFd, fwd.report());
      else if (cmd == "rss") write_line(replyFd, std::to_string(status_kb((int)getpid(), "VmRSS")));
    }
    const double now = mono_seconds();
    if (now >= nextPoll)
    {
      for (auto &c : clients) c->poll();
      nextPoll = now + 0.03;
    }
  }
  for (auto &t : audio) t->stop();
  for (auto &c : clients) c->stop();
  fwd.close();
  _exit(0);
}

struct Room
{
  pid_t pid = -1;
  int cmd = -1, reply = -1;
  uint16_t port = 0;

  bool start(int others, double rate, int block, int kbps, int frame, double loss)
  {
    int down[2], up[2];
    if (pipe(down) || pipe(up)) return false;
    fflush(stdout);
    fflush(stderr);
    pid = fork();
    if (pid < 0) return false;
    if (pid == 0)
    {
      ::close(down[1]);
      ::close(up[0]);
      run_room(others, rate, block, kbps, frame, loss, down[0], up[1]);
    }
    ::close(down[0]);
    ::close(up[1]);
    cmd = down[1];
    reply = up[0];
    std::string line;
    if (!read_line(reply, &line, 10.0) || line.compare(0, 5, "port "))
    {
      say("live_bench: the room didn't start: %s\n", line.c_str());
      return false;
    }
    port = (uint16_t)atoi(line.c_str() + 5);
    return true;
  }

  std::string ask(const std::string &what)
  {
    write_line(cmd, what);
    std::string line;
    read_line(reply, &line, 5.0);
    return line;
  }

  void stop()
  {
    if (pid <= 0) return;
    write_line(cmd, "quit");
    int status = 0;
    waitpid(pid, &status, 0);
    ::close(cmd);
    ::close(reply);
    pid = -1;
  }
};

struct Traffic
{
  double upD = 0, upB = 0, upHello = 0, upData = 0, upOther = 0, downD = 0, downB = 0;
};

static Traffic traffic_of(const std::string &report, uint32_t id)
{
  Traffic t;
  size_t at = 0;
  while (at < report.size())
  {
    size_t e = report.find(';', at);
    if (e == std::string::npos) e = report.size();
    unsigned int pid = 0;
    unsigned long long v[7] = {};
    if (sscanf(report.c_str() + at, "%u %llu %llu %llu %llu %llu %llu %llu", &pid, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5],
               &v[6]) == 8 && pid == id)
    {
      t.upD = (double)v[0];
      t.upB = (double)v[1];
      t.upHello = (double)v[2];
      t.upData = (double)v[3];
      t.upOther = (double)v[4];
      t.downD = (double)v[5];
      t.downB = (double)v[6];
    }
    at = e + 1;
  }
  return t;
}

// One run with our Client and 'others' players: its numbers, and (rss) its memory each second.
struct RunResult
{
  bool ok = false;
  std::string why;
  int blocks = 0;
  std::vector<double> wall, cpu, late;
  double audioCpuPct = 0, rxCpuPct = 0, txCpuPct = 0, mainCpuPct = 0;
  bool audioRt = false, txRt = false;
  int healthyLooks = 0, looks = 0, dropouts = 0;
  double delayMs = 0;
  Traffic traffic;
  double seconds = 0;
  jamsa_live::Counters counters;
};

static RunResult run_loopback(int others, double rate, int block, int kbps, int frame, double loss, double seconds,
                              std::function<void(double t)> eachSecond = nullptr)
{
  RunResult res;
  Room room;
  if (!room.start(others, rate, block, kbps, frame, loss))
  {
    res.why = "the room didn't start";
    room.stop();
    return res;
  }
  {
    Client client;
    const std::vector<long> before = task_ids();
    std::string err;
    if (!client.start(config_for(1, room.port, kbps, frame), &err))
    {
      res.why = "start: " + err;
      room.stop();
      return res;
    }
    // the Client's two threads: the receive thread starts first; the send thread raises its priority
    std::vector<long> mine;
    for (long t : task_ids())
      if (std::find(before.begin(), before.end(), t) == before.end()) mine.push_back(t);
    long rxTid = 0, txTid = 0;
    if (mine.size() == 2)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      if (task_policy(mine[0]) != 0 && task_policy(mine[1]) == 0) std::swap(mine[0], mine[1]);
      rxTid = mine[0];
      txTid = mine[1];
    }
    client.setPeers(peers_except(1, others + 1));
    AudioThread audio;
    audio.start(&client, rate, block, 1, seconds + 5);
    res.audioRt = audio.realtime.load();
    res.txRt = txTid && task_policy(txTid) != 0;

    // everybody heard, and healthy
    auto all_healthy = [&]() {
      const std::vector<PeerStats> st = client.stats();
      int h = 0;
      for (const PeerStats &s : st) if (s.healthy) h++;
      return h == others;
    };
    const double t0 = mono_seconds();
    bool ok = false;
    while (mono_seconds() - t0 < 30)
    {
      client.poll();
      if (all_healthy() && mono_seconds() - t0 > 2.0)
      {
        ok = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    if (!ok)
    {
      res.why = "not everybody healthy after 30 s";
    }
    else
    {
      // a few seconds of playing well first
      const double settle = mono_seconds() + 3.0;
      while (mono_seconds() < settle)
      {
        client.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
      }
      int drops0 = 0;
      for (const PeerStats &s : client.stats()) drops0 += s.underruns + s.overruns + s.dropped;
      room.ask("mark");
      const long mainTid = (long)getpid();
      const int64_t a0 = task_cpu_ns(audio.tid.load()), r0 = task_cpu_ns(rxTid), x0 = task_cpu_ns(txTid),
                    m0 = task_cpu_ns(mainTid);
      const double w0 = mono_seconds();
      audio.recording.store(true);
      double nextSecond = w0 + 1.0;
      while (mono_seconds() - w0 < seconds)
      {
        client.poll();
        res.looks++;
        if (all_healthy()) res.healthyLooks++;
        if (eachSecond && mono_seconds() >= nextSecond)
        {
          eachSecond(nextSecond - w0);
          nextSecond += 1.0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
      }
      audio.recording.store(false);
      const double w1 = mono_seconds();
      const int64_t a1 = task_cpu_ns(audio.tid.load()), r1 = task_cpu_ns(rxTid), x1 = task_cpu_ns(txTid),
                    m1 = task_cpu_ns(mainTid);
      res.traffic = traffic_of(room.ask("report"), 1);
      res.seconds = w1 - w0;
      auto pctOf = [&](int64_t x, int64_t y) { return x >= 0 && y >= 0 ? 100.0 * (double)(y - x) * 1e-9 / res.seconds : -1.0; };
      res.audioCpuPct = pctOf(a0, a1);
      res.rxCpuPct = pctOf(r0, r1);
      res.txCpuPct = pctOf(x0, x1);
      res.mainCpuPct = pctOf(m0, m1);
      const size_t k = audio.count.load(std::memory_order_acquire);
      res.blocks = (int)k;
      for (size_t i = 0; i < k; i++)
      {
        res.wall.push_back(audio.wallUs[i]);
        res.cpu.push_back(audio.cpuUs[i]);
        res.late.push_back(audio.lateUs[i]);
      }
      int drops1 = 0;
      double delay = 0;
      int n = 0;
      for (const PeerStats &s : client.stats())
      {
        drops1 += s.underruns + s.overruns + s.dropped;
        delay += s.oneWayMs;
        n++;
      }
      res.dropouts = drops1 - drops0;
      res.delayMs = n ? delay / n : 0;
      res.counters = client.counters();
      res.ok = true;
    }
    audio.stop();
    client.stop();
  }
  room.stop();
  return res;
}

static int mode_cpu(const Args &a)
{
  const std::vector<double> listeners = a.nums("listeners", "1,3,5,7");
  const std::vector<double> blocks = a.nums("blocks", "64,128,256,512");
  const double seconds = a.num("seconds", 20), rate = a.num("rate", 48000);
  const int reps = (int)a.num("reps", 3), kbps = (int)a.num("kbps", 128), frame = (int)a.num("frame", 240);
  const std::string samples = a.str("samples", "");
  Csv out(a.str("out", "-"));
  const char *header = "block,listeners,rep,rate,kbps,frame,seconds,blocks,block_us,wall_us_med,wall_us_p90,wall_us_p99,wall_us_p999,"
           "wall_us_max,wall_us_mean,cpu_us_med,cpu_us_p99,cpu_us_mean,pct_of_block_med,pct_of_block_mean,"
           "pct_of_block_p99,late_us_med,late_us_max,audio_thread_cpu_pct,rx_cpu_pct,tx_cpu_pct,main_cpu_pct,"
           "audio_rt,tx_rt,healthy_pct,dropouts,estimate_ms,up_dgram_s,up_hello_s,up_data_s,up_other_s,"
           "up_payload_kbps,up_wire_kbps,down_dgram_s,down_payload_kbps,down_wire_kbps,client_sent,client_received";
  out.line("%s", header);
  const int fields = (int)std::count(header, header + strlen(header), ',') + 1;
  FILE *sf = samples.empty() ? nullptr : fopen(samples.c_str(), "w");
  if (sf) fprintf(sf, "block,listeners,rep,i,wall_us,cpu_us,late_us\n");
  for (double b : blocks)
    for (double l : listeners)
      for (int rep = 1; rep <= reps; rep++)
      {
        const int block = (int)b, others = (int)l;
        RunResult r = run_loopback(others, rate, block, kbps, frame, 0, seconds);
        if (!r.ok)
        {
          say("cpu block %d listeners %d rep %d: %s\n", block, others, rep, r.why.c_str());
          out.line("%d,%d,%d,%.0f,%d,%d,%.1f%s", block, others, rep, rate, kbps, frame, seconds,
                   std::string((size_t)(fields - 7), ',').c_str());
          continue;
        }
        const double blockUs = 1e6 * block / rate;
        std::vector<double> pctBlock;
        for (double w : r.wall) pctBlock.push_back(100.0 * w / blockUs);
        const Traffic &t = r.traffic;
        const double s = r.seconds, kbit = 8.0 / 1000.0;
        out.line("%d,%d,%d,%.0f,%d,%d,%.1f,%d,%.1f,%.2f,%.2f,%.2f,%.2f,%.1f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.1f,%.1f,"
                 "%.2f,%.2f,%.2f,%.2f,%d,%d,%.1f,%d,%.2f,%.1f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%llu,%llu",
                 block, others, rep, rate, kbps, frame, s, r.blocks, blockUs, pct(r.wall, 50), pct(r.wall, 90),
                 pct(r.wall, 99), pct(r.wall, 99.9), pct(r.wall, 100), mean(r.wall), pct(r.cpu, 50), pct(r.cpu, 99),
                 mean(r.cpu), pct(pctBlock, 50), mean(pctBlock), pct(pctBlock, 99), pct(r.late, 50), pct(r.late, 100),
                 r.audioCpuPct, r.rxCpuPct, r.txCpuPct, r.mainCpuPct, r.audioRt ? 1 : 0, r.txRt ? 1 : 0,
                 r.looks ? 100.0 * r.healthyLooks / r.looks : 0.0, r.dropouts, r.delayMs, t.upD / s, t.upHello / s,
                 t.upData / s, t.upOther / s, t.upB / s * kbit, (t.upB + 28 * t.upD) / s * kbit, t.downD / s,
                 t.downB / s * kbit, (t.downB + 28 * t.downD) / s * kbit, (unsigned long long)r.counters.sent,
                 (unsigned long long)r.counters.received);
        if (sf)
          for (size_t i = 0; i < r.wall.size(); i++)
            fprintf(sf, "%d,%d,%d,%zu,%.2f,%.2f,%.1f\n", block, others, rep, i, r.wall[i], r.cpu[i], r.late[i]);
        say("cpu block %d listeners %d rep %d: %.1f us median (%.2f%% of the block), p99 %.1f us, max %.0f us; "
            "audio thread %.2f%%, receive %.2f%%, send %.2f%% of a core; up %.0f/s %.1f kbit/s, down %.0f/s %.1f kbit/s; "
            "%d dropouts\n",
            block, others, rep, pct(r.wall, 50), pct(pctBlock, 50), pct(r.wall, 99), pct(r.wall, 100), r.audioCpuPct,
            r.rxCpuPct, r.txCpuPct, t.upD / s, t.upB / s * kbit, t.downD / s, t.downB / s * kbit, r.dropouts);
      }
  if (sf) fclose(sf);
  return 0;
}

static int mode_rss(const Args &a)
{
  const int others = (int)a.num("listeners", 7), block = (int)a.num("block", 256);
  const double seconds = a.num("seconds", 120), rate = a.num("rate", 48000), loss = a.num("loss", 0) / 100.0;
  Csv out(a.str("out", "-"));
  out.line("t_s,rss_kb,rss_anon_kb,hwm_kb,listeners,block,loss_pct");
  const int pid = (int)getpid();
  out.line("%.0f,%ld,%ld,%ld,%d,%d,%.1f", -1.0, status_kb(pid, "VmRSS"), status_kb(pid, "RssAnon"), status_kb(pid, "VmHWM"),
           others, block, loss * 100);
  RunResult r = run_loopback(others, rate, block, 128, 240, loss, seconds, [&](double t) {
    out.line("%.0f,%ld,%ld,%ld,%d,%d,%.1f", t, status_kb(pid, "VmRSS"), status_kb(pid, "RssAnon"), status_kb(pid, "VmHWM"),
             others, block, loss * 100);
  });
  if (!r.ok)
  {
    say("rss: %s\n", r.why.c_str());
    return 1;
  }
  say("rss: %d listeners, %.0f s, healthy %.1f%%, %d dropouts; after the run RSS %ld kB\n", others, r.seconds,
      r.looks ? 100.0 * r.healthyLooks / r.looks : 0.0, r.dropouts, status_kb(pid, "VmRSS"));
  return 0;
}

// ---------------------------------------------------------------------------
// relay: load for the real server (bench_live.py logs the players in)
// ---------------------------------------------------------------------------

#ifdef __linux__

static const uint32_t kBenchMagic = 0x484e4542; // "BENH"

struct RelayPlayer
{
  uint32_t id = 0;
  uint64_t key = 0;
  int room = 0;
  int fd = -1;
  std::atomic<uint64_t> cookie{0};
  std::atomic<bool> helloNow{false};
  std::atomic<int> acks{0};
  uint32_t seq = 0;
  uint32_t lastOverflow = 0; // SO_RXQ_OVFL's count (receive thread)
};

// latency histogram: 1 us bins to 50 ms, then 1 ms bins to 5 s
struct Hist
{
  static const int kFine = 50000, kCoarse = 5000;
  std::vector<uint64_t> fine = std::vector<uint64_t>(kFine, 0), coarse = std::vector<uint64_t>(kCoarse + 1, 0);
  uint64_t n = 0, negative = 0;
  double sum = 0, max = 0;
  void add(double us)
  {
    if (us < 0)
    {
      negative++;
      us = 0;
    }
    n++;
    sum += us;
    max = std::max(max, us);
    if (us < kFine) fine[(size_t)us]++;
    else coarse[std::min((size_t)(us / 1000.0), (size_t)kCoarse)]++;
  }
  void merge(const Hist &h)
  {
    for (int i = 0; i < kFine; i++) fine[i] += h.fine[i];
    for (int i = 0; i <= kCoarse; i++) coarse[i] += h.coarse[i];
    n += h.n;
    negative += h.negative;
    sum += h.sum;
    max = std::max(max, h.max);
  }
  double quantile(double q) const
  {
    if (!n) return NAN;
    const uint64_t want = (uint64_t)ceil(q * (double)n);
    uint64_t c = 0;
    for (int i = 0; i < kFine; i++)
    {
      c += fine[i];
      if (c >= want) return i + 0.5;
    }
    for (int i = 0; i <= kCoarse; i++)
    {
      c += coarse[i];
      if (c >= want) return i * 1000.0 + 500.0;
    }
    return max;
  }
};

static int mode_relay(const Args &a)
{
  const int port = (int)a.num("port", 0);
  const double seconds = a.num("seconds", 20), warm = a.num("warm", 3);
  const int aooBytes = std::max(16, std::min((int)wire::kMaxPayload, (int)a.num("aoo-bytes", 105)));
  const int uniBytes = std::max(16, std::min((int)wire::kMaxPayload, (int)a.num("uni-bytes", 45)));
  const double dataRate = a.num("data-rate", 200), uniRate = a.num("uni-rate", 4), helloEvery = a.num("hello", 2);
  const double farRate = a.num("uni-far-rate", 1);
  const int uniNear = std::max(0, (int)a.num("uni-near", 8));
  const std::string label = a.str("label", "");
  if (port <= 0)
  {
    say("live_bench relay: --port P\n");
    return 2;
  }
  // the players
  std::vector<std::unique_ptr<RelayPlayer>> players;
  char line[256];
  while (fgets(line, sizeof(line), stdin))
  {
    if (!strncmp(line, "end", 3)) break;
    unsigned int id = 0, room = 0;
    unsigned long long key = 0;
    if (sscanf(line, "%u %llu %u", &id, &key, &room) != 3) continue;
    std::unique_ptr<RelayPlayer> p(new RelayPlayer);
    p->id = id;
    p->key = key;
    p->room = (int)room;
    players.push_back(std::move(p));
  }
  const int P = (int)players.size();
  if (P < 2)
  {
    say("live_bench relay: fewer than 2 players on stdin\n");
    return 2;
  }
  std::map<int, int> roomSize;
  for (auto &p : players) roomSize[p->room]++;
  std::map<uint32_t, RelayPlayer *> byId;
  for (auto &p : players) byId[p->id] = p.get();

  prctl(PR_SET_TIMERSLACK, 1UL, 0, 0, 0);
  sockaddr_in srv;
  memset(&srv, 0, sizeof(srv));
  srv.sin_family = AF_INET;
  srv.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  srv.sin_port = htons((uint16_t)port);
  for (auto &p : players)
  {
    p->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (p->fd < 0 || bind(p->fd, (const sockaddr *)&me, sizeof(me)) || connect(p->fd, (const sockaddr *)&srv, sizeof(srv)))
    {
      say("live_bench relay: socket: %s\n", strerror(errno));
      return 1;
    }
    int big = 8 << 20, on = 1;
    if (setsockopt(p->fd, SOL_SOCKET, SO_RCVBUFFORCE, &big, sizeof(big)))
      setsockopt(p->fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    setsockopt(p->fd, SOL_SOCKET, SO_TIMESTAMPNS, &on, sizeof(on));
    setsockopt(p->fd, SOL_SOCKET, SO_RXQ_OVFL, &on, sizeof(on));
    fcntl(p->fd, F_SETFL, fcntl(p->fd, F_GETFL) | O_NONBLOCK);
  }

  auto send_hello = [](RelayPlayer &p, uint64_t stamp) {
    uint8_t buf[wire::kHelloSize];
    const size_t n = wire::writeHello(buf, p.id, p.key, stamp, p.cookie.load());
    return send(p.fd, buf, n, 0) == (ssize_t)n;
  };

  // receiving: two threads, each with half the sockets
  std::atomic<bool> quit{false};
  std::atomic<int64_t> measureFrom{INT64_MAX}, measureTo{INT64_MAX};
  const int nrx = P >= 8 ? 2 : 1;
  std::vector<Hist> hists(nrx);
  std::vector<uint64_t> gotData(nrx, 0), gotUni(nrx, 0), gotOutside(nrx, 0), bad(nrx, 0);
  std::vector<std::thread> rx;
  for (int t = 0; t < nrx; t++)
  {
    rx.push_back(std::thread([&, t] {
      const int ep = epoll_create1(0);
      for (int i = t; i < P; i += nrx)
      {
        epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.ptr = players[i].get();
        epoll_ctl(ep, EPOLL_CTL_ADD, players[i]->fd, &ev);
      }
      const int V = 64;
      std::vector<std::vector<uint8_t>> bufs(V, std::vector<uint8_t>(wire::kMaxDatagram + 1));
      std::vector<std::vector<char>> ctl(V, std::vector<char>(256));
      mmsghdr msgs[V];
      iovec iov[V];
      epoll_event evs[64];
      while (!quit.load(std::memory_order_relaxed))
      {
        const int ne = epoll_wait(ep, evs, 64, 20);
        for (int e = 0; e < ne; e++)
        {
          RelayPlayer &p = *(RelayPlayer *)evs[e].data.ptr;
          for (;;)
          {
            for (int k = 0; k < V; k++)
            {
              iov[k].iov_base = bufs[k].data();
              iov[k].iov_len = bufs[k].size();
              memset(&msgs[k].msg_hdr, 0, sizeof(msgs[k].msg_hdr));
              msgs[k].msg_hdr.msg_iov = &iov[k];
              msgs[k].msg_hdr.msg_iovlen = 1;
              msgs[k].msg_hdr.msg_control = ctl[k].data();
              msgs[k].msg_hdr.msg_controllen = ctl[k].size();
            }
            const int n = recvmmsg(p.fd, msgs, V, MSG_DONTWAIT, nullptr);
            if (n <= 0) break;
            const int64_t from = measureFrom.load(std::memory_order_relaxed), to = measureTo.load(std::memory_order_relaxed);
            for (int k = 0; k < n; k++)
            {
              int64_t rxNs = 0;
              for (cmsghdr *c = CMSG_FIRSTHDR(&msgs[k].msg_hdr); c; c = CMSG_NXTHDR(&msgs[k].msg_hdr, c))
              {
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS)
                {
                  timespec ts;
                  memcpy(&ts, CMSG_DATA(c), sizeof(ts));
                  rxNs = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
                }
                else if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL)
                {
                  uint32_t o;
                  memcpy(&o, CMSG_DATA(c), sizeof(o));
                  p.lastOverflow = o;
                }
              }
              wire::FromServer m;
              if (!wire::parseFromServer(bufs[k].data(), msgs[k].msg_len, m))
              {
                bad[t]++;
                continue;
              }
              if (m.type == wire::kHelloAck)
              {
                if (m.cookie != p.cookie.load())
                {
                  p.cookie.store(m.cookie);
                  p.helloNow.store(true);
                }
                p.acks++;
                continue;
              }
              if (m.size < 21)
              {
                bad[t]++;
                continue;
              }
              uint32_t magic, sender, seq;
              int64_t txNs;
              memcpy(&magic, m.payload, 4);
              memcpy(&sender, m.payload + 4, 4);
              memcpy(&seq, m.payload + 8, 4);
              memcpy(&txNs, m.payload + 12, 8);
              const uint8_t kind = m.payload[20];
              if (magic != kBenchMagic)
              {
                bad[t]++;
                continue;
              }
              if (txNs < from || txNs >= to)
              {
                gotOutside[t]++;
                continue;
              }
              if (kind == 0) gotData[t]++;
              else gotUni[t]++;
              hists[t].add(rxNs ? (double)(rxNs - txNs) / 1000.0 : -1.0);
            }
            if (n < V) break;
          }
        }
      }
      ::close(ep);
    }));
  }

  // binding: a HELLO, then one with the cookie the server answered
  for (int attempt = 0; attempt < 5; attempt++)
  {
    int bound = 0;
    for (auto &p : players)
    {
      if (p->acks.load() >= 2 && p->cookie.load()) { bound++; continue; }
      send_hello(*p, ((uint64_t)1 << 63) | (uint64_t)realtime_ns());
    }
    if (bound == P) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
  int bound = 0;
  for (auto &p : players) if (p->acks.load() >= 2) bound++;
  if (bound < P) say("live_bench relay: only %d of %d players got their cookie\n", bound, P);

  // sending: each player's datagrams evenly spread, all players' interleaved
  std::vector<uint64_t> sentData(1, 0), sentUni(1, 0), expected(1, 0), sendFailed(1, 0);
  const double start = mono_seconds() + 0.05;
  const double measureStart = start + warm, measureEnd = measureStart + seconds, stopAt = measureEnd + 0.5;
  const int64_t realStart = realtime_ns() + (int64_t)((measureStart - mono_seconds()) * 1e9);
  measureFrom.store(realStart);
  measureTo.store(realStart + (int64_t)(seconds * 1e9));
  struct Due
  {
    double data, uni, hello;
    int nextPeer;
  };
  std::vector<Due> due(P);
  for (int i = 0; i < P; i++)
  {
    due[i].data = start + (double)i / ((double)P * dataRate);
    due[i].uni = 1e300; // set below, with the roommates
    due[i].hello = start + helloEvery * (double)i / P;
    due[i].nextPeer = 0;
  }
  // each player's unicasts: --uni-rate a second to each of the first --uni-near others in its room (the
  // players it listens to), --uni-far-rate to each of the rest, interleaved in one cycle
  std::vector<std::vector<RelayPlayer *>> roommates(P);
  std::vector<double> uniTotal(P, 0);
  const int reps = farRate > 0 ? std::max(1, (int)lround(uniRate / farRate)) : 1;
  for (int i = 0; i < P; i++)
  {
    std::vector<RelayPlayer *> near, far;
    for (int j = 0; j < P; j++)
      if (j != i && players[j]->room == players[i]->room) ((int)near.size() < uniNear ? near : far).push_back(players[j].get());
    for (int r = 0; r < reps; r++)
    {
      if (uniRate > 0) roommates[i].insert(roommates[i].end(), near.begin(), near.end());
      if (farRate > 0)
        roommates[i].insert(roommates[i].end(), far.begin() + (ptrdiff_t)(far.size() * r / reps),
                            far.begin() + (ptrdiff_t)(far.size() * (r + 1) / reps));
    }
    uniTotal[i] = (double)near.size() * uniRate + (double)far.size() * farRate;
  }
  for (int i = 0; i < P; i++)
    if (uniTotal[i] > 0 && !roommates[i].empty()) due[i].uni = start + (double)i / ((double)P * uniTotal[i]);
  const double dataStep = 1.0 / dataRate;
  uint8_t buf[wire::kMaxDatagram];
  double sendBusy = 0;
  bool saidStart = false, saidEnd = false; // for bench_live.py, which samples the server's CPU in between
  for (;;)
  {
    const double now = mono_seconds();
    if (!saidStart && now >= measureStart)
    {
      say("measure start\n");
      saidStart = true;
    }
    if (!saidEnd && now >= measureEnd)
    {
      say("measure end\n");
      saidEnd = true;
    }
    if (now >= stopAt) break;
    double next = std::min(stopAt, saidStart ? measureEnd : measureStart);
    if (saidEnd) next = stopAt;
    for (int i = 0; i < P; i++)
    {
      RelayPlayer &p = *players[i];
      Due &d = due[i];
      if (p.helloNow.exchange(false) || now >= d.hello)
      {
        send_hello(p, ((uint64_t)1 << 63) | (uint64_t)realtime_ns());
        if (now >= d.hello) d.hello += helloEvery;
      }
      while (now >= d.data)
      {
        const int64_t tx = realtime_ns();
        wire::writeDataHeader(buf, p.id, p.key, 0);
        uint8_t *pl = buf + wire::kClientHeaderSize;
        memset(pl, 0, (size_t)aooBytes);
        memcpy(pl, &kBenchMagic, 4);
        memcpy(pl + 4, &p.id, 4);
        memcpy(pl + 8, &p.seq, 4);
        memcpy(pl + 12, &tx, 8);
        pl[20] = 0;
        p.seq++;
        const size_t n = wire::kClientHeaderSize + (size_t)aooBytes;
        const bool ok = send(p.fd, buf, n, 0) == (ssize_t)n;
        if (tx >= measureFrom.load() && tx < measureTo.load())
        {
          if (ok)
          {
            sentData[0]++;
            expected[0] += (uint64_t)(roomSize[p.room] - 1);
          }
          else sendFailed[0]++;
        }
        d.data += dataStep;
        // a sender that fell far behind starts again from now, as a player's send thread would
        if (now - d.data > 0.1) d.data = now + dataStep;
      }
      while (now >= d.uni && !roommates[i].empty())
      {
        RelayPlayer *q = roommates[i][d.nextPeer % roommates[i].size()];
        d.nextPeer++;
        const int64_t tx = realtime_ns();
        wire::writeDataHeader(buf, p.id, p.key, q->id);
        uint8_t *pl = buf + wire::kClientHeaderSize;
        memset(pl, 0, (size_t)uniBytes);
        memcpy(pl, &kBenchMagic, 4);
        memcpy(pl + 4, &p.id, 4);
        memcpy(pl + 8, &p.seq, 4);
        memcpy(pl + 12, &tx, 8);
        pl[20] = 1;
        const size_t n = wire::kClientHeaderSize + (size_t)uniBytes;
        const bool ok = send(p.fd, buf, n, 0) == (ssize_t)n;
        if (tx >= measureFrom.load() && tx < measureTo.load())
        {
          if (ok)
          {
            sentUni[0]++;
            expected[0]++;
          }
          else sendFailed[0]++;
        }
        d.uni += 1.0 / uniTotal[i];
        if (now - d.uni > 0.1) d.uni = now;
      }
      next = std::min(next, std::min(d.data, std::min(d.uni, d.hello)));
    }
    const double after = mono_seconds();
    sendBusy += after - now;
    if (next > after) sleep_until(next);
  }
  // what is still on its way
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  quit.store(true);
  for (auto &t : rx) t.join();
  Hist h;
  uint64_t got = 0, gotU = 0, outside = 0, bads = 0, overflow = 0;
  for (int t = 0; t < nrx; t++)
  {
    h.merge(hists[t]);
    got += gotData[t];
    gotU += gotUni[t];
    outside += gotOutside[t];
    bads += bad[t];
  }
  for (auto &p : players) overflow += p->lastOverflow;
  const uint64_t received = got + gotU;
  const double lostPct = expected[0] ? 100.0 * (double)((int64_t)expected[0] - (int64_t)received) / (double)expected[0] : 0.0;
  const std::string out = a.str("out", "-");
  FILE *f = stdout;
  bool header = true;
  if (out != "-")
  {
    FILE *probe = fopen(out.c_str(), "r");
    if (probe)
    {
      header = fgetc(probe) == EOF;
      fclose(probe);
    }
    f = fopen(out.c_str(), "a");
    if (!f) f = stdout;
  }
  if (header)
    fprintf(f, "label,players,rooms,seconds,data_rate,aoo_bytes,uni_rate,uni_near,uni_far_rate,uni_bytes,sent_data,sent_uni,send_failed,"
               "expected_copies,received_copies,lost_pct,rx_overflow,lat_p50_us,lat_p90_us,lat_p99_us,lat_p999_us,"
               "lat_max_us,lat_mean_us,in_dgram_s,out_dgram_s,sender_busy_pct,bound\n");
  fprintf(f, "%s,%d,%d,%.1f,%.1f,%d,%.2f,%d,%.2f,%d,%llu,%llu,%llu,%llu,%llu,%.4f,%llu,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%d\n",
          label.c_str(), P, (int)roomSize.size(), seconds, dataRate, aooBytes, uniRate, uniNear, farRate, uniBytes,
          (unsigned long long)sentData[0], (unsigned long long)sentUni[0], (unsigned long long)sendFailed[0],
          (unsigned long long)expected[0], (unsigned long long)received, lostPct, (unsigned long long)overflow,
          h.quantile(0.5), h.quantile(0.9), h.quantile(0.99), h.quantile(0.999), h.max, h.n ? h.sum / h.n : NAN,
          (double)(sentData[0] + sentUni[0]) / seconds, (double)received / seconds,
          100.0 * sendBusy / (stopAt - start), bound);
  if (f != stdout) fclose(f);
  const std::string histPath = a.str("hist", "");
  if (!histPath.empty())
  {
    FILE *hf = fopen(histPath.c_str(), "w");
    if (hf)
    {
      fprintf(hf, "label,lat_us_from,lat_us_to,count\n");
      for (int i = 0; i < Hist::kFine; i++)
        if (h.fine[i]) fprintf(hf, "%s,%d,%d,%llu\n", label.c_str(), i, i + 1, (unsigned long long)h.fine[i]);
      for (int i = 0; i <= Hist::kCoarse; i++)
        if (h.coarse[i]) fprintf(hf, "%s,%d,%d,%llu\n", label.c_str(), i * 1000, (i + 1) * 1000, (unsigned long long)h.coarse[i]);
      fclose(hf);
    }
  }
  say("relay %s: %d players in %d rooms: %llu copies expected, %llu received (%.3f%% lost, %llu dropped at our sockets), "
      "latency p50 %.0f us, p99 %.0f us, p99.9 %.0f us, max %.0f us\n",
      label.c_str(), P, (int)roomSize.size(), (unsigned long long)expected[0], (unsigned long long)received, lostPct,
      (unsigned long long)overflow, h.quantile(0.5), h.quantile(0.99), h.quantile(0.999), h.max);
  for (auto &p : players) ::close(p->fd);
  return 0;
}

#else

static int mode_relay(const Args &)
{
  say("live_bench relay: Linux only\n");
  return 2;
}

#endif

int main(int argc, char **argv)
{
  signal(SIGPIPE, SIG_IGN);
  if (argc < 2)
  {
    say("usage: live_bench net|bw|cpu|rss|relay [--name value ...] (see live_bench.cpp)\n");
    return 2;
  }
  Args a(argc, argv);
  if (!a.ok) return 2;
  const std::string mode = argv[1];
  if (mode == "net") return mode_net(a);
  if (mode == "bw") return mode_bw(a);
  if (mode == "cpu") return mode_cpu(a);
  if (mode == "rss") return mode_rss(a);
  if (mode == "relay") return mode_relay(a);
  say("live_bench: unknown mode %s\n", mode.c_str());
  return 2;
}
