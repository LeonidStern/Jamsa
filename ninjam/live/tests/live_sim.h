/*
    Jamsa Live - live_sim.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Players (each a jamsa_live::Core, as in the plug-in) and a server in one
  process, on a virtual clock and a virtual network, for the Live tests.

  Each player runs host blocks at its own sample rate, block size and clock
  drift; its time stamps are the true (virtual) time, as a computer's clock
  next to an audio interface that runs a little fast or slow. After each
  block its send() runs, as the send thread would (also while the host
  doesn't call processBlock()). Its datagrams carry our UDP header
  (wire::) to the server, which checks them and copies them to the room
  exactly as the real one does (its header written in place at offset 12),
  and the copies go through each player's down link.

  Each direction of each player's path has its own delay, jitter (which
  reorders), random loss, loss bursts and duplicates, all from one seeded
  generator: the same seed gives the same run. Datagrams wait in a queue
  ordered by delivery time and reach AOO in heap blocks of exactly their
  size, so that AddressSanitizer sees any read past the end.

*/

#ifndef JAMSA_LIVE_SIM_H
#define JAMSA_LIVE_SIM_H

#include "livecore.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include <chrono>
#include <functional>
#include <memory>
#include <new>
#include <queue>
#include <random>
#include <vector>

namespace live_sim {

using jamsa_live::Core;
namespace wire = jamsa_live::wire;

// one direction of a player's path to the server
struct link
{
  double delay = 0.005;    // seconds
  double jitter = 0;       // up to this much more, uniformly (reorders)
  double loss = 0;         // each datagram on its own
  double burst = 0;        // the chance, per datagram, that a loss burst starts
  double burst_len = 0.03; // a burst's length in seconds
  double dup = 0;          // the chance of a second copy
  double burst_until = -1;
  long lost = 0;
};

struct player
{
  uint32_t id = 0;
  uint64_t key = 0;
  std::unique_ptr<Core> core;
  double rate = 48000;
  double ppm = 0;            // its audio clock against true time
  int block = 128;
  double t0 = 0;             // true time of its first block
  int64_t nblocks = 0;
  bool in_room = true;       // the server copies to it
  bool host_running = true;  // the host calls processBlock() (the send thread runs anyway)
  // a host that is late and then catches up: the blocks due in [late_from, late_to) run at
  // late_to, one right after the other
  double late_from = 0, late_to = 0;
  // a host that calls in bursts: 'burst' blocks at once, when the last one is due (set it only
  // once: back to 1, the rest of a burst would be due before now)
  int burst = 1;
  bool send = true;          // processBlock's 'send'
  bool send_throws = false;  // the send function throws std::bad_alloc
  link up, down;
  double tone_hz = 440;
  // the input from sample 'index' on (its own clock); a tone by default
  std::function<void(int64_t index, float *l, float *r, int n)> input;
  std::vector<float> inL, inR;
  // every slot's left channel while 'recording'
  bool recording = false;
  std::vector<float> rec[Core::kMaxPeers];
  std::vector<double> rec_time; // true time of each recorded block
  double block_ms = 0;          // how long the last processBlock() took (wall clock)
  // what went through the server
  long uploads = 0, upload_bytes = 0, unicast_uploads = 0, downloads = 0;

  double eff_rate() const { return rate * (1 + ppm * 1e-6); }
  // when this block's audio is due (its input in, its output out), and when the host calls for it
  double due_block() const { return t0 + (double)nblocks * block / eff_rate(); }
  double next_block() const
  {
    double t = burst > 1 ? t0 + (double)(nblocks / burst * burst + burst - 1) * block / eff_rate() : due_block();
    if (t >= late_from && t < late_to) t = late_to;
    return t;
  }
  double sample_time(int64_t index) const { return t0 + (double)index / eff_rate(); }

  void clear_recording()
  {
    for (auto &r : rec) r.clear();
    rec_time.clear();
  }
  // true time of recorded sample i
  double rec_sample_time(size_t i) const
  {
    return rec_time[i / block] + (double)(i % block) / eff_rate();
  }
};

struct net
{
  double now = 0;
  std::mt19937 rng;
  std::vector<std::unique_ptr<player>> players;
  double poll_interval = 0.03;
  double next_poll = 0;
  // the server
  long server_rx = 0, server_bad = 0, forwarded = 0, client_bad = 0;
  // sees each valid DATA datagram at the server; false drops it
  std::function<bool(const wire::FromClient &)> server_filter;
  // sees each copy before its down link; false drops it
  std::function<bool(uint32_t from, uint32_t to, const uint8_t *payload, size_t size)> copy_filter;

  explicit net(unsigned seed = 1) : rng(seed) {}

  double uniform() { return std::uniform_real_distribution<double>(0, 1)(rng); }

  player &add(uint32_t id, double rate = 48000, int block = 128)
  {
    std::unique_ptr<player> p(new player);
    p->id = id;
    p->key = 0x9E3779B97F4A7C15ull * id;
    p->rate = rate;
    p->block = block;
    p->t0 = now + 0.00037 * id; // the players' blocks don't line up
    p->tone_hz = 200 + 100 * id;
    p->core.reset(new Core);
    p->core->setSelf(id);
    p->core->prepare(rate, block);
    p->inL.resize(block);
    p->inR.resize(block);
    players.push_back(std::move(p));
    return *players.back();
  }

  player *find(uint32_t id)
  {
    for (auto &p : players) if (p->id == id) return p.get();
    return nullptr;
  }

  // the room's other players, in order of joining
  std::vector<jamsa_live::Peer> peers_of(const player &p) const
  {
    std::vector<jamsa_live::Peer> out;
    for (auto &q : players)
    {
      if (q->id == p.id || !q->in_room) continue;
      jamsa_live::Peer peer;
      peer.id = q->id;
      peer.name = "p" + std::to_string(q->id);
      peer.bound = true;
      out.push_back(peer);
    }
    return out;
  }

  // everybody in the room hears everybody and streams
  void start_all()
  {
    for (auto &p : players)
    {
      p->core->setPeers(peers_of(*p));
      p->core->setRoomOn(true);
      p->core->setStreaming(true);
    }
  }

  void update_peers()
  {
    for (auto &p : players) if (p->in_room) p->core->setPeers(peers_of(*p));
  }

  // ------------------------------------------------------------------
  // the network

  struct packet
  {
    double at;
    uint64_t seq;
    uint32_t to; // 0 = the server
    std::vector<uint8_t> bytes;
  };
  struct later
  {
    bool operator()(const packet &a, const packet &b) const
    {
      return a.at > b.at || (a.at == b.at && a.seq > b.seq);
    }
  };
  std::priority_queue<packet, std::vector<packet>, later> queue;
  uint64_t seq = 0;

  void push(double at, uint32_t to, const std::vector<uint8_t> &bytes)
  {
    packet p;
    p.at = at;
    p.seq = seq++;
    p.to = to;
    p.bytes = bytes;
    queue.push(std::move(p));
  }

  // through a link: lost, or delivered (maybe twice)
  void transmit(link &l, uint32_t to, const std::vector<uint8_t> &bytes)
  {
    if (l.burst_until > now) { l.lost++; return; }
    if (l.burst > 0 && uniform() < l.burst)
    {
      l.burst_until = now + l.burst_len;
      l.lost++;
      return;
    }
    if (l.loss > 0 && uniform() < l.loss) { l.lost++; return; }
    push(now + l.delay + l.jitter * uniform(), to, bytes);
    if (l.dup > 0 && uniform() < l.dup) push(now + l.delay + l.jitter * uniform(), to, bytes);
  }

  struct send_context
  {
    net *n;
    player *p;
  };

  static void on_send(void *user, uint32_t dest, const uint8_t *data, size_t size)
  {
    send_context *c = (send_context *)user;
    player &p = *c->p;
    std::vector<uint8_t> d(wire::kClientHeaderSize + size);
    wire::writeDataHeader(d.data(), p.id, p.key, dest);
    memcpy(d.data() + wire::kClientHeaderSize, data, size);
    p.uploads++;
    p.upload_bytes += (long)d.size();
    if (dest) p.unicast_uploads++;
    c->n->transmit(p.up, 0, d);
  }

  static void throw_send(void *, uint32_t, const uint8_t *, size_t)
  {
    throw std::bad_alloc();
  }

  void server(const packet &pk)
  {
    server_rx++;
    wire::FromClient m;
    player *s = nullptr;
    if (!wire::parseFromClient(pk.bytes.data(), pk.bytes.size(), m) || m.type != wire::kData
        || !(s = find(m.sender)) || s->key != m.key || !s->in_room)
    {
      server_bad++;
      return;
    }
    if (server_filter && !server_filter(m)) return;
    for (auto &q : players)
    {
      if (q->id == m.sender || !q->in_room || (m.dest && q->id != m.dest)) continue;
      if (copy_filter && !copy_filter(m.sender, q->id, m.payload, m.size)) continue;
      // the server's in-place rewrite: its 8 bytes at offset 12, sent from there
      std::vector<uint8_t> buf(pk.bytes);
      wire::writeServerHeader(buf.data() + 12, wire::kData, m.sender);
      std::vector<uint8_t> out(buf.begin() + 12, buf.end());
      forwarded++;
      transmit(q->down, q->id, out);
    }
  }

  void deliver(const packet &pk)
  {
    player *q = find(pk.to);
    if (!q) return;
    wire::FromServer m;
    if (!wire::parseFromServer(pk.bytes.data(), pk.bytes.size(), m) || m.type != wire::kData)
    {
      client_bad++;
      return;
    }
    q->downloads++;
    // a heap block of exactly the payload's size
    std::vector<uint8_t> payload(m.payload, m.payload + m.size);
    // (on the clock of run_block()'s time stamps)
    q->core->handleData(m.from, payload.data(), payload.size(), jamsa_live::ntpFromSeconds(now + 1000.0));
  }

  static void tone(player &p, int64_t index, float *l, float *r, int n)
  {
    const double w = 6.283185307179586 * p.tone_hz / p.rate;
    for (int i = 0; i < n; i++)
    {
      const double ph = w * (double)(index + i);
      l[i] = (float)(0.5 * sin(ph));
      r[i] = (float)(0.25 * sin(ph));
    }
  }

  void run_block(player &p)
  {
    const double t = p.next_block();
    if (p.host_running)
    {
      const int64_t index = p.nblocks * (int64_t)p.block;
      if (p.input) p.input(index, p.inL.data(), p.inR.data(), p.block);
      else tone(p, index, p.inL.data(), p.inR.data(), p.block);
      // (an offset, so that no time stamp is near 0)
      const auto w0 = std::chrono::steady_clock::now();
      p.core->processBlock(p.inL.data(), p.inR.data(), p.block, p.send,
                           jamsa_live::ntpFromSeconds(t + 1000.0));
      p.block_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
      if (p.recording)
      {
        for (int k = 0; k < Core::kMaxPeers; k++)
        {
          const float *s = p.core->stem(k, 0);
          p.rec[k].insert(p.rec[k].end(), s, s + p.block);
        }
        p.rec_time.push_back(p.due_block());
      }
    }
    // the send thread, which runs whether or not the host calls
    send_context c = { this, &p };
    p.core->send(p.send_throws ? throw_send : on_send, &c);
    p.nblocks++;
  }

  // to whatever comes next: a block, a datagram, a poll
  void step()
  {
    double t = next_poll;
    player *next = nullptr;
    for (auto &p : players)
    {
      const double b = p->next_block();
      if (b < t)
      {
        t = b;
        next = p.get();
      }
    }
    if (!queue.empty() && queue.top().at <= t)
    {
      packet pk = queue.top();
      queue.pop();
      if (pk.at > now) now = pk.at;
      if (pk.to == 0) server(pk);
      else deliver(pk);
      return;
    }
    now = t;
    if (next)
    {
      run_block(*next);
      return;
    }
    for (auto &p : players) p->core->poll(now);
    next_poll = now + poll_interval;
  }

  void run(double seconds)
  {
    const double end = now + seconds;
    while (now < end) step();
  }

  template <class F> bool run_until(F done, double timeout)
  {
    const double end = now + timeout;
    while (now < end)
    {
      if (done()) return true;
      step();
    }
    return done();
  }
};

} // namespace live_sim

#endif
