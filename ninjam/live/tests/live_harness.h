/*
    Jamsa Live - live_harness.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  An AOO source and a few sinks in one process, on a virtual clock and a
  virtual network, for the Live tests.

  Every node (the source is node 0, the sinks 1..n) has a made-up IPv4
  address and runs its blocks at its own sample rate and block size; tick()
  advances to whichever node is due next, delivers the datagrams that have
  arrived by then, runs that node's process(), send() and pollEvents(), and
  queues what it sent. The time AOO sees is the virtual clock too, so a test
  runs as fast as the CPU allows and the same way every time.

  A test can look at, change, drop or add datagrams through 'filter'. Each
  datagram is handed to AOO in a heap block of exactly its size, so that
  AddressSanitizer catches any read past its end.

  With source_options::broadcast the source runs in Jamsa's broadcast mode:
  what it sends to 'broadcast_addr' reaches every sink, as through our
  server (each copy goes through 'filter' on its own).

*/

#ifndef JAMSA_LIVE_HARNESS_H
#define JAMSA_LIVE_HARNESS_H

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include "aoo.h"
#include "aoo_sink.hpp"
#include "aoo_source.hpp"
#include "codec/aoo_opus.h"

#include <string.h>

#include <cmath>

#include <functional>
#include <memory>
#include <vector>

namespace live_harness {

struct datagram
{
  int from, to;          // node indexes
  double due;            // arrival time in seconds
  std::vector<AooByte> bytes;
  bool broadcast = false; // a copy of what the source sent to broadcast_addr
};

struct node
{
  sockaddr_in addr;
  AooId id = 1;
  double samplerate = 48000;
  int blocksize = 64;
  int channels = 2;
  double next_time = 0;
  long blocks = 0;
  std::vector<std::vector<AooSample>> buffers;
  std::vector<AooSample *> pointers;

  // what the node's events said
  int events[32] = {};
  int stream_active = 0;      // kAooStreamStateActive events
  int last_active_offset = -1;
  int dropped = 0, resent = 0, xrun = 0;
  std::vector<AooByte> start_metadata; // of the last kAooEventStreamStart
  bool start_had_metadata = false;
  long stream_messages = 0;

  AooEndpoint endpoint() const
  {
    AooEndpoint ep;
    ep.address = &addr;
    ep.addrlen = sizeof(addr);
    ep.id = id;
    return ep;
  }
};

inline sockaddr_in make_address(int index)
{
  sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(1);
  a.sin_addr.s_addr = htonl(0x0A000000u + 1 + (uint32_t)index); // 10.0.0.x
  return a;
}

inline AooNtpTime ntp_from_seconds(double s)
{
  // an arbitrary start in 2023, then seconds in 32.32 fixed point
  return ((AooNtpTime)3900000000u << 32) + (AooNtpTime)(s * 4294967296.0);
}

struct sink_options
{
  double samplerate = 48000;
  int blocksize = 64;
  bool dynamic_resampling = false;
  double latency = 0.05; // seconds
};

struct source_options
{
  double samplerate = 48000;
  int blocksize = 64;
  int format_blocksize = 240;  // Opus frame: 5 ms at 48 kHz
  bool binary = true;
  bool add_sinks = true;       // add every sink to the source up front
  bool broadcast = false;      // Jamsa's broadcast mode, to broadcast_addr
  int bitrate = 0;             // Opus bitrate in the format (0: Opus' default)
};

class session
{
public:
  AooSource::Ptr source;
  std::vector<AooSink::Ptr> sinks; // sinks[i] is node i + 1
  std::vector<node> nodes;
  std::vector<datagram> in_flight;
  double now = 0;
  double network_delay = 0.002;
  // return false to drop the datagram; it may change d.bytes or d.due
  std::function<bool(datagram &d)> filter;
  // the source's input: a 440 Hz sine on the left, 660 Hz on the right
  double amplitude = 0.5;
  bool silent = false;
  long datagrams_sent = 0;
  // the source's audio thread misses this many blocks (an xrun); its
  // network thread keeps going
  int source_skips = 0;
  // pass the sinks a stream message handler (AOO allows none)
  bool message_handler = true;
  // called with true right before a node's process() and with false right
  // after it: what runs in between is what an audio thread would run
  void (*audio_hook)(bool) = nullptr;
  // broadcast mode: the source's datagrams to this address go to every sink
  sockaddr_in broadcast_addr = make_address(250);
  long broadcast_sent = 0; // datagrams the source sent there

  session(const source_options &so, const std::vector<sink_options> &sos)
  {
    nodes.resize(1 + sos.size());
    for (size_t i = 0; i < nodes.size(); i++) nodes[i].addr = make_address((int)i);

    node &src = nodes[0];
    src.samplerate = so.samplerate;
    src.blocksize = so.blocksize;
    alloc_buffers(src);
    source = AooSource::create(1);
    source->setEventHandler(on_event, &src, kAooEventModePoll);
    source->setup(2, so.samplerate, so.blocksize, kAooFixedBlockSize);
    source->setBinaryFormat(so.binary);
    AooFormatOpus fmt;
    AooFormatOpus_init(&fmt, 2, 48000, so.format_blocksize, OPUS_APPLICATION_RESTRICTED_LOWDELAY);
    if (so.bitrate > 0) fmt.bitrate = so.bitrate;
    source->setFormat(fmt.header);
    if (so.broadcast)
    {
      AooEndpoint ep;
      ep.address = &broadcast_addr;
      ep.addrlen = sizeof(broadcast_addr);
      ep.id = 1;
      source->setBroadcastEndpoint(&ep);
    }

    for (size_t i = 0; i < sos.size(); i++)
    {
      node &n = nodes[i + 1];
      n.samplerate = sos[i].samplerate;
      n.blocksize = sos[i].blocksize;
      alloc_buffers(n);
      AooSink::Ptr s = AooSink::create(1);
      s->setEventHandler(on_event, &n, kAooEventModePoll);
      s->setup(2, sos[i].samplerate, sos[i].blocksize, kAooFixedBlockSize);
      s->setDynamicResampling(sos[i].dynamic_resampling);
      s->setLatency(sos[i].latency);
      s->setBinaryFormat(so.binary);
      if (so.add_sinks) source->addSink(n.endpoint(), true);
      sinks.push_back(std::move(s));
    }
  }

  void start(const AooData *metadata = nullptr, int offset = 0)
  {
    source->startStream(offset, metadata);
  }

  // run until 'seconds' of virtual time have passed
  void run(double seconds)
  {
    const double end = now + seconds;
    while (now < end) tick();
  }

  // run until cond() holds, at most 'seconds'; true if it held
  template<typename F> bool run_until(F cond, double seconds)
  {
    const double end = now + seconds;
    while (now < end)
    {
      tick();
      if (cond()) return true;
    }
    return false;
  }

  // runs the node that is due next and returns its index
  int tick()
  {
    // the node that is due first
    size_t k = 0;
    for (size_t i = 1; i < nodes.size(); i++)
      if (nodes[i].next_time < nodes[k].next_time) k = i;
    node &n = nodes[k];
    now = n.next_time;
    n.blocks++;
    n.next_time = (double)n.blocks * n.blocksize / n.samplerate;

    deliver(now);

    const AooNtpTime t = ntp_from_seconds(now);
    if (k == 0)
    {
      fill_input(n);
      if (source_skips > 0) source_skips--;
      else
      {
        if (audio_hook) audio_hook(true);
        source->process(n.pointers.data(), n.blocksize, t);
        if (audio_hook) audio_hook(false);
      }
      sender ctx = { this, 0 };
      source->send(send_function, &ctx);
      source->pollEvents();
    }
    else
    {
      AooSink *s = sinks[k - 1].get();
      if (audio_hook) audio_hook(true);
      s->process(n.pointers.data(), n.blocksize, t,
        message_handler ? on_stream_message : nullptr, &n);
      if (audio_hook) audio_hook(false);
      sender ctx = { this, (int)k };
      s->send(send_function, &ctx);
      s->pollEvents();
      check_output(n);
    }
    return (int)k;
  }

  // the RMS of a sink's last block, both channels
  double block_rms(int sink_node) const
  {
    const node &n = nodes[sink_node];
    double sum = 0;
    for (int c = 0; c < n.channels; c++)
      for (int i = 0; i < n.blocksize; i++) sum += (double)n.buffers[c][i] * n.buffers[c][i];
    return std::sqrt(sum / (n.channels * n.blocksize));
  }

  bool outputs_finite = true;
  double peak = 0;

  // hand a datagram to node 'to' now, as if node 'from' had sent it
  AooError inject(int from, int to, const AooByte *data, size_t size)
  {
    return inject_from(nodes[from].addr, to, data, size);
  }

  // the same from any address
  AooError inject_from(const sockaddr_in &a, int to, const AooByte *data, size_t size)
  {
    // an exact-size heap copy, so ASan sees reads past the end
    std::unique_ptr<AooByte[]> copy(new AooByte[size ? size : 1]);
    if (size) memcpy(copy.get(), data, size);
    if (to == 0) return source->handleMessage(copy.get(), (AooInt32)size, &a, sizeof(a));
    return sinks[to - 1]->handleMessage(copy.get(), (AooInt32)size, &a, sizeof(a));
  }

private:
  struct sender { session *self; int from; };

  static void alloc_buffers(node &n)
  {
    n.buffers.assign(n.channels, std::vector<AooSample>(n.blocksize, 0));
    n.pointers.clear();
    for (auto &b : n.buffers) n.pointers.push_back(b.data());
  }

  void fill_input(node &n)
  {
    const double two_pi = 6.283185307179586;
    for (int i = 0; i < n.blocksize; i++)
    {
      const double t = (double)((n.blocks - 1) * n.blocksize + i) / n.samplerate;
      n.buffers[0][i] = silent ? 0 : (AooSample)(amplitude * std::sin(two_pi * 440 * t));
      n.buffers[1][i] = silent ? 0 : (AooSample)(amplitude * std::sin(two_pi * 660 * t));
    }
  }

  void check_output(node &n)
  {
    for (int c = 0; c < n.channels; c++)
      for (int i = 0; i < n.blocksize; i++)
      {
        const double v = n.buffers[c][i];
        if (!std::isfinite(v)) outputs_finite = false;
        else if (std::fabs(v) > peak) peak = std::fabs(v);
      }
  }

  void deliver(double t)
  {
    // in order of arrival; datagrams due at the same time keep their order
    for (;;)
    {
      size_t best = in_flight.size();
      for (size_t i = 0; i < in_flight.size(); i++)
        if (in_flight[i].due <= t && (best == in_flight.size() || in_flight[i].due < in_flight[best].due))
          best = i;
      if (best == in_flight.size()) break;
      datagram d = std::move(in_flight[best]);
      in_flight.erase(in_flight.begin() + best);
      inject(d.from, d.to, d.bytes.data(), d.bytes.size());
    }
  }

  int node_of(const void *address, AooAddrSize addrlen) const
  {
    if (addrlen != sizeof(sockaddr_in)) return -1;
    const sockaddr_in *a = (const sockaddr_in *)address;
    for (size_t i = 0; i < nodes.size(); i++)
      if (nodes[i].addr.sin_addr.s_addr == a->sin_addr.s_addr && nodes[i].addr.sin_port == a->sin_port)
        return (int)i;
    return -1;
  }

  static AooInt32 AOO_CALL send_function(void *user, const AooByte *data, AooInt32 size,
                                         const void *address, AooAddrSize addrlen, AooFlag)
  {
    sender *ctx = (sender *)user;
    session *self = ctx->self;
    if (addrlen == sizeof(sockaddr_in)
      && ((const sockaddr_in *)address)->sin_addr.s_addr == self->broadcast_addr.sin_addr.s_addr)
    {
      // one copy for every sink, as our server does
      self->datagrams_sent++;
      self->broadcast_sent++;
      for (int to = 1; to < (int)self->nodes.size(); to++)
      {
        datagram d;
        d.from = ctx->from;
        d.to = to;
        d.due = self->now + self->network_delay;
        d.bytes.assign(data, data + size);
        d.broadcast = true;
        if (self->filter && !self->filter(d)) continue;
        self->in_flight.push_back(std::move(d));
      }
      return size;
    }
    const int to = self->node_of(address, addrlen);
    if (to < 0) return 0; // nobody there
    datagram d;
    d.from = ctx->from;
    d.to = to;
    d.due = self->now + self->network_delay;
    d.bytes.assign(data, data + size);
    self->datagrams_sent++;
    if (self->filter && !self->filter(d)) return size;
    self->in_flight.push_back(std::move(d));
    return size;
  }

  static void AOO_CALL on_event(void *user, const AooEvent *e, AooThreadLevel)
  {
    node &n = *(node *)user;
    if (e->type >= 0 && e->type < 32) n.events[e->type]++;
    switch (e->type)
    {
      case kAooEventStreamState:
        if (e->streamState.state == kAooStreamStateActive)
        {
          n.stream_active++;
          n.last_active_offset = e->streamState.sampleOffset;
        }
      break;
      case kAooEventStreamStart:
        n.start_had_metadata = e->streamStart.metadata != nullptr;
        if (e->streamStart.metadata)
          n.start_metadata.assign(e->streamStart.metadata->data,
            e->streamStart.metadata->data + e->streamStart.metadata->size);
      break;
      case kAooEventBlockDrop: n.dropped += e->blockDrop.count; break;
      case kAooEventBlockResend: n.resent += e->blockResend.count; break;
      case kAooEventBlockXRun: n.xrun += e->blockXRun.count; break;
      default: break;
    }
  }

  static void AOO_CALL on_stream_message(void *user, const AooStreamMessage *, const AooEndpoint *)
  {
    ((node *)user)->stream_messages++;
  }
};

} // namespace live_harness

#endif
