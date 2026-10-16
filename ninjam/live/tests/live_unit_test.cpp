/*
    Jamsa Live - live_unit_test.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Live mode's own additions to AOO, on a virtual clock and network:

  - Opus: one coupled stream for stereo (and mono stays one stream), the
    bitrate given with the format, and OPUS_GET_BITRATE;
  - broadcast mode: a source sends each data packet once, to an address
    that passes it on to every sink, with /start, /stop, pings and resends
    still per sink. Three sinks.
  - AooSink::removeSource(), for a sink that gets a new peer;
  - the glue (livecore.h), with players on a virtual network through a
    server (live_sim.h): the datagrams, a room of four, how late they hear
    each other, loss (resent or concealed), sample rates, clock drift over
    five minutes, slot reuse, the filter in front of AOO, fuzzed packets,
    the room's Live flag, a host that stops calling, bitrate and frame
    changes. live_client_test runs the Client on real sockets.

  Run with a test name to run only the tests whose names start with it.

*/

#include "detail.hpp" // aoo::find_codec()

#include "live_harness.h"
#include "live_sim.h"
#include "testutil.h"

#include "opus.h"
#include "osc/OscOutboundPacketStream.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#include <random>
#include <string>
#include <vector>

using namespace live_harness;

static const char *g_only = nullptr;

static bool selected(const char *name)
{
  if (g_only && strncmp(name, g_only, strlen(g_only))) return false;
  printf("%s\n", name);
  fflush(stdout);
  return true;
}

// ---------------------------------------------------------------------------
// Signal helpers
// ---------------------------------------------------------------------------

// Fits a*sin + b*cos + c at 'hz' to x by least squares and returns the
// power of the fitted tone over the power of what is left, in dB.
static double tone_snr(const float *x, size_t n, double hz, double sr)
{
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
  // Gauss-Jordan on the 3x3 normal equations
  for (int c = 0; c < 3; c++)
  {
    int p = c;
    for (int r = c + 1; r < 3; r++) if (fabs(m[r][c]) > fabs(m[p][c])) p = r;
    for (int k = 0; k < 4; k++) { const double t = m[c][k]; m[c][k] = m[p][k]; m[p][k] = t; }
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
    const double e = x[i] - t - dc;
    res += e * e;
  }
  if (res <= 0) return 200;
  return 10 * log10(sig / res);
}

// a binary /data message to a sink
static bool is_data(const std::vector<AooByte> &b)
{
  return b.size() >= 4 && b[0] == (0x80 | kAooMsgTypeSink)
    && (b[1] & 0x7f) == kAooBinMsgCmdData;
}

// ---------------------------------------------------------------------------
// Opus: coupled stereo, bitrate
// ---------------------------------------------------------------------------

struct codec_ptr
{
  AooCodec *c = nullptr;
  bool encoder = true;
  ~codec_ptr()
  {
    if (!c) return;
    if (encoder) c->cls->encoderFree(c);
    else c->cls->decoderFree(c);
  }
};

static AooFormatOpus opus_format(int channels, int bitrate)
{
  AooFormatOpus f;
  AooFormatOpus_init(&f, channels, 48000, 240, OPUS_APPLICATION_RESTRICTED_LOWDELAY);
  f.bitrate = bitrate;
  return f;
}

// encodes 'frames' Opus frames of 'channels' sines (500 Hz * (channel + 1))
// and decodes them again; returns the packets, the decoded audio per channel
static void opus_roundtrip(int channels, int bitrate, int frames,
                           std::vector<std::vector<AooByte>> &packets,
                           std::vector<std::vector<float>> &decoded)
{
  const AooCodecInterface *codec = aoo::find_codec(kAooCodecOpus);
  CHECK(codec != nullptr);
  if (!codec) return;
  codec_ptr enc, dec;
  enc.c = codec->encoderNew();
  dec.c = codec->decoderNew();
  dec.encoder = false;
  AooFormatOpus f = opus_format(channels, bitrate);
  CHECK_EQ(AooEncoder_setup(enc.c, &f.header), kAooOk);
  // the sink's format has no bitrate (it isn't sent), as after deserialize()
  AooFormatOpus g = f;
  g.header.structSize = AOO_STRUCT_SIZE(AooFormatOpus, applicationType);
  CHECK_EQ(AooDecoder_setup(dec.c, &g.header), kAooOk);

  packets.clear();
  decoded.assign(channels, std::vector<float>());
  std::vector<float> in(240 * channels), out(240 * channels);
  long pos = 0;
  for (int k = 0; k < frames; k++)
  {
    for (int i = 0; i < 240; i++, pos++)
      for (int c = 0; c < channels; c++)
        in[i * channels + c] = (float)(0.4 * sin(6.283185307179586 * 500.0 * (c + 1) * (double)pos / 48000.0));
    AooByte buf[4000];
    AooInt32 size = sizeof(buf);
    CHECK_EQ(AooEncoder_encode(enc.c, in.data(), 240, buf, &size), kAooOk);
    packets.emplace_back(buf, buf + size);
    AooInt32 n = 240;
    CHECK_EQ(AooDecoder_decode(dec.c, buf, size, out.data(), &n), kAooOk);
    CHECK_EQ(n, 240);
    for (int i = 0; i < 240; i++)
      for (int c = 0; c < channels; c++) decoded[c].push_back(out[i * channels + c]);
  }
}

static void test_opus_coupled_stereo()
{
  if (!selected("opus_coupled_stereo")) return;
  for (int channels = 1; channels <= 3; channels++)
  {
    std::vector<std::vector<AooByte>> packets;
    std::vector<std::vector<float>> decoded;
    opus_roundtrip(channels, 64000 * channels, 200, packets, decoded);
    if (packets.empty()) continue;
    // A stereo stream is one coupled Opus stream: its packets are plain
    // stereo Opus packets. (Upstream sent two mono streams, the first
    // self-delimited, so the first TOC byte said mono.)
    int stereo_toc = 0;
    for (auto &p : packets) if (opus_packet_get_nb_channels(p.data()) == 2) stereo_toc++;
    if (channels == 1) CHECK_EQ(stereo_toc, 0);
    if (channels == 2) CHECK_EQ(stereo_toc, (int)packets.size());
    if (channels == 3) CHECK_EQ(stereo_toc, (int)packets.size()); // coupled pair first
    // every channel comes back as itself (the decoder's layout matches)
    for (int c = 0; c < channels; c++)
    {
      const std::vector<float> &x = decoded[c];
      const size_t skip = x.size() / 2; // well past the codec delay
      const double snr = tone_snr(x.data() + skip, x.size() - skip, 500.0 * (c + 1), 48000);
      printf("  %d channels: channel %d SNR %.1f dB\n", channels, c, snr);
      CHECK(snr > 20);
    }
  }
}

// average bytes of an encoded 5 ms frame of stereo noise
static double opus_average_bytes(AooCodec *enc, int frames, unsigned seed)
{
  std::minstd_rand rng(seed);
  std::uniform_real_distribution<float> u(-0.3f, 0.3f);
  std::vector<float> in(480);
  double total = 0;
  for (int k = 0; k < frames; k++)
  {
    for (auto &v : in) v = u(rng);
    AooByte buf[4000];
    AooInt32 size = sizeof(buf);
    AooEncoder_encode(enc, in.data(), 240, buf, &size);
    total += size;
  }
  return total / frames;
}

static void test_opus_bitrate()
{
  if (!selected("opus_bitrate")) return;
  const AooCodecInterface *codec = aoo::find_codec(kAooCodecOpus);
  CHECK(codec != nullptr);
  if (!codec) return;
  for (int bitrate : { 32000, 128000, 256000 })
  {
    codec_ptr enc;
    enc.c = codec->encoderNew();
    AooFormatOpus f = opus_format(2, bitrate);
    CHECK_EQ(AooEncoder_setup(enc.c, &f.header), kAooOk);
    opus_int32 got = 0;
    CHECK_EQ(AooEncoder_control(enc.c, OPUS_GET_BITRATE_REQUEST, AOO_ARG(got)), kAooOk);
    CHECK_EQ(got, bitrate);
    const double bytes = opus_average_bytes(enc.c, 400, 1);
    const double target = bitrate / 8.0 / 200.0; // 200 frames a second
    printf("  %d bit/s: %.1f bytes a frame (target %.1f)\n", bitrate, bytes, target);
    CHECK(bytes > target * 0.75 && bytes < target * 1.25);

    // a change through the codec control holds, and reads back
    opus_int32 lower = bitrate / 2;
    CHECK_EQ(AooEncoder_control(enc.c, OPUS_SET_BITRATE_REQUEST, AOO_ARG(lower)), kAooOk);
    CHECK_EQ(AooEncoder_control(enc.c, OPUS_GET_BITRATE_REQUEST, AOO_ARG(got)), kAooOk);
    CHECK_EQ(got, lower);
    const double half = opus_average_bytes(enc.c, 400, 2);
    CHECK(half > target * 0.5 * 0.75 && half < target * 0.5 * 1.25);
  }
  // a format without the field (or with OPUS_AUTO) leaves Opus' default
  {
    codec_ptr enc;
    enc.c = codec->encoderNew();
    AooFormatOpus f = opus_format(2, 99999);
    f.header.structSize = AOO_STRUCT_SIZE(AooFormatOpus, applicationType);
    CHECK_EQ(AooEncoder_setup(enc.c, &f.header), kAooOk);
    opus_int32 got = 0;
    CHECK_EQ(AooEncoder_control(enc.c, OPUS_GET_BITRATE_REQUEST, AOO_ARG(got)), kAooOk);
    CHECK_EQ(got, OPUS_AUTO);
  }
  // a bitrate Opus refuses fails the setup
  {
    codec_ptr enc;
    enc.c = codec->encoderNew();
    AooFormatOpus f = opus_format(2, -5);
    CHECK(AooEncoder_setup(enc.c, &f.header) != kAooOk);
  }
}

// the source's data packets follow the format's bitrate
static void test_opus_bitrate_source()
{
  if (!selected("opus_bitrate_source")) return;
  double average[2] = {};
  const int rates[2] = { 32000, 256000 };
  for (int k = 0; k < 2; k++)
  {
    source_options so;
    so.bitrate = rates[k];
    session s(so, { sink_options() });
    long bytes = 0, count = 0;
    s.filter = [&](datagram &d) {
      if (d.from == 0 && is_data(d.bytes)) { bytes += (long)d.bytes.size(); count++; }
      return true;
    };
    s.start();
    s.run(2.0);
    opus_int32 got = 0;
    CHECK_EQ(s.source->codecControl(kAooCodecOpus, OPUS_GET_BITRATE_REQUEST, 0, AOO_ARG(got)), kAooOk);
    CHECK_EQ(got, rates[k]);
    CHECK(count > 300);
    average[k] = count ? (double)bytes / count : 0;
    printf("  %d bit/s: %.1f bytes a data packet\n", rates[k], average[k]);
  }
  // tones are cheap to code, so less than the 8 times; but clearly more
  CHECK(average[1] > average[0] * 2);
}

// ---------------------------------------------------------------------------
// Broadcast mode
// ---------------------------------------------------------------------------

// records every sink's left channel, from 'from' seconds on
struct recorder
{
  session &s;
  double from;
  std::vector<std::vector<float>> left;
  recorder(session &s_, double from_) : s(s_), from(from_), left(s_.sinks.size()) {}
  void run(double seconds)
  {
    const double end = s.now + seconds;
    while (s.now < end)
    {
      const int k = s.tick();
      if (k > 0 && s.now >= from)
        left[k - 1].insert(left[k - 1].end(), s.nodes[k].buffers[0].begin(), s.nodes[k].buffers[0].end());
    }
  }
};

struct data_counter
{
  long broadcast = 0;  // copies of broadcast data packets, all sinks
  long unicast[8] = {};  // data packets the source sent to one sink
  void count(const datagram &d)
  {
    if (d.from != 0 || !is_data(d.bytes)) return;
    if (d.broadcast) broadcast++;
    else if (d.to < 8) unicast[d.to]++;
  }
};

static void test_broadcast_three_sinks()
{
  if (!selected("broadcast_three_sinks")) return;
  long uploads[2] = {};
  for (int bc = 0; bc < 2; bc++)
  {
    source_options so;
    so.broadcast = bc == 1;
    sink_options si;
    si.latency = 0.03;
    session s(so, { si, si, si });
    data_counter n;
    s.filter = [&](datagram &d) { n.count(d); return true; };
    s.start();
    recorder r(s, 1.0);
    r.run(3.0);
    for (int k = 1; k <= 3; k++)
    {
      CHECK_EQ(s.nodes[k].stream_active, 1);
      CHECK_EQ(s.nodes[k].dropped, 0);
      const double snr = tone_snr(r.left[k - 1].data(), r.left[k - 1].size(), 440, 48000);
      CHECK(snr > 20);
      if (bc) CHECK_EQ(n.unicast[k], 0);
      else CHECK(n.unicast[k] > 500);
    }
    if (bc)
    {
      // 3 s of 5 ms blocks, sent once, reaching all three sinks
      CHECK(s.broadcast_sent > 550 && s.broadcast_sent < 650);
      CHECK_EQ(n.broadcast, s.broadcast_sent * 3);
      uploads[1] = s.broadcast_sent;
    }
    else
    {
      CHECK_EQ(s.broadcast_sent, 0);
      CHECK_EQ(n.broadcast, 0);
      uploads[0] = n.unicast[1] + n.unicast[2] + n.unicast[3];
    }
    printf("  %s: %ld data packets sent\n", bc ? "broadcast" : "per sink", uploads[bc]);
  }
  CHECK(uploads[0] > uploads[1] * 29 / 10);
}

// A copy lost on the way to one sink is resent to that sink only.
static void test_broadcast_resend()
{
  if (!selected("broadcast_resend")) return;
  source_options so;
  so.broadcast = true;
  session s(so, { sink_options(), sink_options(), sink_options() });
  data_counter n;
  long lost = 0, copies = 0;
  s.filter = [&](datagram &d) {
    if (d.broadcast && d.to == 2 && s.now > 0.5 && (++copies % 10) == 0) { lost++; return false; }
    n.count(d);
    return true;
  };
  s.start();
  s.run(4.0);
  printf("  lost %ld copies to sink 2; resent %d; unicast data %ld/%ld/%ld\n",
    lost, s.nodes[2].resent, n.unicast[1], n.unicast[2], n.unicast[3]);
  CHECK(lost > 50);
  CHECK(s.nodes[2].resent >= lost - 2); // the last ones may still be on their way
  CHECK_EQ(s.nodes[2].dropped, 0);
  CHECK(n.unicast[2] >= lost - 2);
  for (int k : { 1, 3 })
  {
    CHECK_EQ(s.nodes[k].resent, 0);
    CHECK_EQ(s.nodes[k].dropped, 0);
    CHECK_EQ(n.unicast[k], 0);
  }
  for (int k = 1; k <= 3; k++) CHECK_EQ(s.nodes[k].stream_active, 1);
  CHECK(s.outputs_finite);
}

// A sink the source doesn't know asks for /start in vain; once added it
// joins the running stream (the shared stream ID) without restarting it
// for the others. A sink with a channel offset gets copies of its own.
static void test_broadcast_late_sink()
{
  if (!selected("broadcast_late_sink")) return;
  source_options so;
  so.broadcast = true;
  so.add_sinks = false;
  session s(so, { sink_options(), sink_options(), sink_options() });
  s.source->addSink(s.nodes[1].endpoint(), true);
  s.source->addSink(s.nodes[2].endpoint(), true);
  data_counter n;
  long start_requests = 0;
  s.filter = [&](datagram &d) {
    n.count(d);
    if (d.from == 3 && d.bytes.size() > 9 && !memcmp(d.bytes.data(), "/aoo/src/", 9)
      && strstr((const char *)d.bytes.data(), "/start")) start_requests++;
    return true;
  };
  s.start();
  s.run(1.0);
  CHECK_EQ(s.nodes[1].stream_active, 1);
  CHECK_EQ(s.nodes[2].stream_active, 1);
  CHECK_EQ(s.nodes[3].stream_active, 0); // it gets the data but no /start
  CHECK(start_requests > 0);

  s.source->addSink(s.nodes[3].endpoint(), true);
  CHECK(s.run_until([&] { return s.nodes[3].stream_active == 1; }, 0.5));
  s.run(0.5);
  CHECK(s.block_rms(3) > 0.2);
  CHECK_EQ(s.nodes[1].stream_active, 1); // no new stream for the others
  CHECK_EQ(s.nodes[2].stream_active, 1);
  for (int k = 1; k <= 3; k++) CHECK_EQ(n.unicast[k], 0);

  // a channel offset needs packets of its own
  const long before = n.unicast[3];
  int32_t offset = 1;
  AooEndpoint ep = s.nodes[3].endpoint();
  CHECK_EQ(s.source->setSinkChannelOffset(ep, offset), kAooOk);
  s.run(0.5);
  CHECK(n.unicast[3] - before > 80);
  CHECK_EQ(n.unicast[1], 0);
  CHECK_EQ(n.unicast[2], 0);
}

// Turning broadcast mode on or off restarts the stream; every sink follows.
static void test_broadcast_toggle()
{
  if (!selected("broadcast_toggle")) return;
  source_options so;
  session s(so, { sink_options(), sink_options(), sink_options() });
  data_counter n;
  s.filter = [&](datagram &d) { n.count(d); return true; };
  s.start();
  s.run(1.0);
  CHECK_EQ(s.broadcast_sent, 0);
  AooEndpoint ep;
  ep.address = &s.broadcast_addr;
  ep.addrlen = sizeof(s.broadcast_addr);
  ep.id = 1;
  CHECK_EQ(s.source->setBroadcastEndpoint(&ep), kAooOk);
  AooBool on = kAooFalse;
  CHECK_EQ(s.source->getBroadcast(on), kAooOk);
  CHECK(on == kAooTrue);
  const long unicast_before = n.unicast[1];
  s.run(1.0);
  CHECK(s.broadcast_sent > 150);
  CHECK(n.unicast[1] - unicast_before < 5); // the old stream's last packets
  for (int k = 1; k <= 3; k++)
  {
    CHECK_EQ(s.nodes[k].stream_active, 2);
    CHECK(s.block_rms(k) > 0.2);
  }
  CHECK_EQ(s.source->setBroadcastEndpoint(nullptr), kAooOk);
  CHECK_EQ(s.source->getBroadcast(on), kAooOk);
  CHECK(on == kAooFalse);
  const long broadcast_before = s.broadcast_sent;
  s.run(1.0);
  CHECK(s.broadcast_sent - broadcast_before < 5);
  for (int k = 1; k <= 3; k++)
  {
    CHECK_EQ(s.nodes[k].stream_active, 3);
    CHECK(s.block_rms(k) > 0.2);
  }
  // bad arguments
  AooEndpoint bad = ep;
  bad.addrlen = 0;
  CHECK(s.source->setBroadcastEndpoint(&bad) != kAooOk);
}

// ---------------------------------------------------------------------------
// AooSink::removeSource()
// ---------------------------------------------------------------------------

static void test_remove_source()
{
  if (!selected("remove_source")) return;
  source_options so;
  session s(so, { sink_options(), sink_options() });
  s.start();
  s.run(1.0);
  CHECK_EQ(s.nodes[1].stream_active, 1);
  AooEndpoint src = s.nodes[0].endpoint();
  AooEndpoint stranger = s.nodes[2].endpoint();
  CHECK_EQ(s.sinks[0]->removeSource(&stranger), kAooErrorNotFound);

  // drop the source's packets to sink 1 for a while: it goes away at once,
  // not after the source timeout
  bool drop = true;
  s.filter = [&](datagram &d) { return !(drop && d.to == 1); };
  for (size_t i = s.in_flight.size(); i-- > 0;)
    if (s.in_flight[i].to == 1) s.in_flight.erase(s.in_flight.begin() + i);
  CHECK_EQ(s.sinks[0]->removeSource(&src), kAooOk);
  s.run(0.05);
  CHECK_EQ(s.nodes[1].events[kAooEventSourceRemove], 1);
  CHECK(s.block_rms(1) == 0);
  CHECK(s.block_rms(2) > 0.2); // the other sink plays on
  CHECK_EQ(s.sinks[0]->removeSource(&src), kAooErrorNotFound);

  // its packets bring it back: a new source asks for /start
  drop = false;
  CHECK(s.run_until([&] { return s.nodes[1].stream_active == 2; }, 0.5));
  s.run(0.2);
  CHECK(s.block_rms(1) > 0.2);

  // all of them
  CHECK_EQ(s.sinks[1]->removeSource(nullptr), kAooOk);
  s.run(0.01);
  CHECK_EQ(s.nodes[2].events[kAooEventSourceRemove], 1);
}

// ---------------------------------------------------------------------------
// The glue (livecore.h): players on a virtual network, through the server
// ---------------------------------------------------------------------------

using live_sim::net;
using live_sim::player;
using jamsa_live::Core;
using jamsa_live::PeerStats;
namespace wire = jamsa_live::wire;

static const PeerStats *stats_of(const std::vector<PeerStats> &all, uint32_t id)
{
  for (const PeerStats &s : all) if (s.id == id) return &s;
  return nullptr;
}

// how well listener q hears p's tone on its slot, over its last 'seconds'
static double heard_snr(const player &q, const player &p, double seconds)
{
  const int slot = q.core->slotOf(p.id);
  if (slot < 0) return -100;
  const std::vector<float> &r = q.rec[slot];
  const size_t n = std::min(r.size(), (size_t)(seconds * q.rate));
  if (n < 1000) return -100;
  // the tone at its true frequency, in the listener's samples
  return tone_snr(r.data() + r.size() - n, n, p.tone_hz * (1 + p.ppm * 1e-6), q.eff_rate());
}

// Hann-shaped bursts of a 1 kHz tone, one every 'period' seconds
struct bursts
{
  double period = 0.2, len = 0.01, hz = 1000;
  void install(player &p)
  {
    const bursts b = *this;
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
  // The delay of each burst that q recorded on p's slot: the centre of its
  // energy, in true time, against the centre of the burst p played.
  std::vector<double> delays(const player &p, const player &q) const
  {
    std::vector<double> out;
    const int slot = q.core->slotOf(p.id);
    if (slot < 0 || q.rec_time.empty()) return out;
    const std::vector<float> &r = q.rec[slot];
    const int64_t P = (int64_t)llround(period * p.rate), L = (int64_t)llround(len * p.rate);
    const double first = p.sample_time(L / 2);         // burst 0's centre
    const double step = (double)P / p.eff_rate();      // between centres
    std::vector<double> e, et;
    for (size_t i = 0; i < r.size(); i++)
    {
      const double t = q.rec_sample_time(i);
      const long m = (long)floor((t - first) / step);
      if (m < 0) continue;
      if ((size_t)m >= e.size()) { e.resize(m + 1, 0); et.resize(m + 1, 0); }
      const double x2 = (double)r[i] * r[i];
      e[m] += x2;
      et[m] += x2 * (t - (first + m * step));
    }
    double emax = 0;
    for (double v : e) emax = std::max(emax, v);
    // not the first and last: they may be cut off
    for (size_t m = 1; m + 1 < e.size(); m++)
      if (e[m] > 0.2 * emax) out.push_back(et[m] / e[m]);
    return out;
  }
};

static double median(std::vector<double> v)
{
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

static double spread(const std::vector<double> &v)
{
  if (v.empty()) return 1e9;
  return *std::max_element(v.begin(), v.end()) - *std::min_element(v.begin(), v.end());
}

static void start_recording(net &n)
{
  for (auto &p : n.players)
  {
    p->clear_recording();
    p->recording = true;
  }
}

// the shares of 10 ms windows of a recording that are (nearly) silent
static double silent_share(const std::vector<float> &r, double rate, double threshold)
{
  const size_t w = (size_t)(0.01 * rate);
  size_t windows = 0, silent = 0;
  for (size_t i = 0; i + w <= r.size(); i += w)
  {
    double sum = 0;
    for (size_t j = 0; j < w; j++) sum += (double)r[i + j] * r[i + j];
    windows++;
    if (sqrt(sum / w) < threshold) silent++;
  }
  return windows ? (double)silent / windows : 1;
}

// the share of 20 ms windows in which the tone comes through clean
static double good_share(const std::vector<float> &r, double hz, double rate, double min_snr)
{
  const size_t w = (size_t)(0.02 * rate);
  size_t windows = 0, good = 0;
  for (size_t i = 0; i + w <= r.size(); i += w)
  {
    windows++;
    if (tone_snr(r.data() + i, w, hz, rate) > min_snr) good++;
  }
  return windows ? (double)good / windows : 0;
}

static bool all_finite(const std::vector<float> &r)
{
  for (float v : r) if (!std::isfinite(v) || fabs(v) > 4) return false;
  return true;
}

// The datagrams of spec section 2: layout, sizes, the server's rewrite.
static void test_glue_wire()
{
  if (!selected("glue_wire")) return;
  uint8_t hello[wire::kHelloSize + 4];
  memset(hello, 0xee, sizeof(hello));
  CHECK_EQ(wire::writeHello(hello, 0x01020304, 0x1122334455667788ull, 0xa1a2a3a4a5a6a7a8ull, 0xc1c2c3c4c5c6c7c8ull), (size_t)40);
  const uint8_t expect[40] = { 'N', 'L', 1, 1, 4, 3, 2, 1, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
                               0, 0, 0, 0, 0xa8, 0xa7, 0xa6, 0xa5, 0xa4, 0xa3, 0xa2, 0xa1,
                               0xc8, 0xc7, 0xc6, 0xc5, 0xc4, 0xc3, 0xc2, 0xc1, 0, 0, 0, 0 };
  CHECK(!memcmp(hello, expect, 40));
  CHECK_EQ(hello[40], 0xee);
  wire::FromClient c;
  CHECK(wire::parseFromClient(hello, 40, c));
  CHECK_EQ(c.type, wire::kHello);
  CHECK_EQ(c.sender, 0x01020304u);
  CHECK_EQ(c.key, 0x1122334455667788ull);
  CHECK_EQ(c.stamp, 0xa1a2a3a4a5a6a7a8ull);
  CHECK_EQ(c.cookie, 0xc1c2c3c4c5c6c7c8ull);
  CHECK(!wire::parseFromClient(hello, 39, c));
  CHECK(!wire::parseFromClient(hello, 41, c));
  hello[16] = 1; // a HELLO has no destination
  CHECK(!wire::parseFromClient(hello, 40, c));
  hello[16] = 0;
  hello[2] = 2; // version
  CHECK(!wire::parseFromClient(hello, 40, c));
  hello[2] = 1;
  hello[0] = 'n';
  CHECK(!wire::parseFromClient(hello, 40, c));

  uint8_t ack[wire::kHelloAckSize];
  CHECK_EQ(wire::writeHelloAck(ack, 0xa1a2a3a4a5a6a7a8ull, 77), (size_t)24);
  wire::FromServer s;
  CHECK(wire::parseFromServer(ack, 24, s));
  CHECK_EQ(s.type, wire::kHelloAck);
  CHECK_EQ(s.stamp, 0xa1a2a3a4a5a6a7a8ull);
  CHECK_EQ(s.cookie, 77u);
  CHECK(!wire::parseFromServer(ack, 23, s));
  CHECK(!wire::parseFromServer(ack, 16, s));
  ack[4] = 1; // from someone
  CHECK(!wire::parseFromServer(ack, 24, s));

  // DATA: 1..1452 bytes of payload; the server writes its header at 12
  std::vector<uint8_t> d(wire::kMaxDatagram + 1, 0x5a);
  wire::writeDataHeader(d.data(), 9, 0xfeedfacecafebeefull, 0);
  CHECK(wire::parseFromClient(d.data(), 20 + 1452, c));
  CHECK_EQ(c.type, wire::kData);
  CHECK_EQ(c.sender, 9u);
  CHECK_EQ(c.dest, 0u);
  CHECK_EQ(c.size, (size_t)1452);
  CHECK(c.payload == d.data() + 20);
  CHECK(!wire::parseFromClient(d.data(), 20 + 1453, c));
  CHECK(!wire::parseFromClient(d.data(), 20, c));
  wire::writeDataHeader(d.data(), 9, 1, 12);
  CHECK(wire::parseFromClient(d.data(), 100, c));
  CHECK_EQ(c.dest, 12u);
  CHECK_EQ(wire::writeServerHeader(d.data() + 12, wire::kData, 9), (size_t)8);
  CHECK(wire::parseFromServer(d.data() + 12, 88, s));
  CHECK_EQ(s.type, wire::kData);
  CHECK_EQ(s.from, 9u);
  CHECK(s.payload == d.data() + 20);
  CHECK_EQ(s.size, (size_t)80);
  CHECK(wire::parseFromServer(d.data() + 12, 8 + 1452, s));
  CHECK(!wire::parseFromServer(d.data() + 12, 8 + 1453, s));
  CHECK(!wire::parseFromServer(d.data() + 12, 8, s));
  wire::writeServerHeader(d.data() + 12, wire::kData, 0); // from no one
  CHECK(!wire::parseFromServer(d.data() + 12, 88, s));
  wire::writeServerHeader(d.data() + 12, 3, 9); // no such type
  CHECK(!wire::parseFromServer(d.data() + 12, 88, s));
  CHECK(!wire::parseFromServer(nullptr, 88, s));
  CHECK(!wire::parseFromServer(d.data(), wire::kMaxDatagram + 1, s));

  // random datagrams, mostly with our magic: nothing accepted points outside
  std::mt19937 rng(7);
  int accepted = 0;
  for (int i = 0; i < 200000; i++)
  {
    size_t size = rng() % 1600;
    std::vector<uint8_t> b(size);
    for (auto &x : b) x = (uint8_t)rng();
    if (size >= 4 && (rng() & 3))
    {
      b[0] = 'N';
      b[1] = 'L';
      b[2] = 1;
      b[3] = 1 + rng() % 2;
      if (size >= 8 && (rng() & 1)) memset(b.data() + 4, 0, 4);
      if (rng() % 4 == 0) b.resize(size = (rng() & 1) ? 24 : 40);
    }
    wire::FromServer fs;
    if (wire::parseFromServer(b.data(), size, fs))
    {
      accepted++;
      if (fs.type == wire::kData)
        CHECK(fs.payload == b.data() + 8 && fs.size == size - 8 && fs.size >= 1 && fs.size <= 1452 && fs.from != 0);
      else
        CHECK(size == 24 && fs.from == 0);
    }
    wire::FromClient fc;
    if (wire::parseFromClient(b.data(), size, fc))
    {
      accepted++;
      if (fc.type == wire::kData)
        CHECK(fc.payload == b.data() + 20 && fc.size == size - 20 && fc.size >= 1 && fc.size <= 1452);
      else
        CHECK(size == 40 && fc.dest == 0);
    }
  }
  CHECK(accepted > 1000);
}

// Four players, each hears the other three: one upload per block each
// (broadcast), not one per listener.
static void test_glue_room_of_four()
{
  if (!selected("glue_room_of_four")) return;
  net n(11);
  for (uint32_t id = 1; id <= 4; id++)
  {
    player &p = n.add(id);
    p.up.jitter = p.down.jitter = 0.002;
  }
  n.start_all();
  n.run(2.0);
  int warm[5][5] = {};
  for (auto &q : n.players)
    for (const PeerStats &ps : q->core->stats(n.now)) warm[q->id][ps.id] = ps.dropped;
  for (auto &p : n.players) p->uploads = p->unicast_uploads = 0;
  start_recording(n);
  n.run(2.0);
  for (auto &q : n.players)
  {
    const std::vector<PeerStats> st = q->core->stats(n.now);
    CHECK_EQ(st.size(), (size_t)3);
    for (auto &p : n.players)
    {
      if (p == q) continue;
      const int slot = q->core->slotOf(p->id);
      CHECK(slot >= 0);
      CHECK(q->core->slotHealthy(slot));
      const double snr = heard_snr(*q, *p, 1.5);
      CHECK(snr > 20);
      const PeerStats *ps = stats_of(st, p->id);
      CHECK(ps != nullptr);
      if (!ps) continue;
      CHECK_EQ(ps->slot, slot);
      CHECK(ps->streaming && ps->healthy);
      // A stream's first /data can overtake its /start (jitter): the sink
      // drops it, asks for the /start, and that block is lost or resent.
      CHECK(ps->dropped <= 1);
      CHECK_EQ(ps->dropped, warm[q->id][p->id]); // none after that
      CHECK_EQ(ps->underruns, 0);
      CHECK_EQ(ps->overruns, 0);
      CHECK(ps->lossPct < 0.5);
      CHECK(ps->bufferMs > 5 && ps->bufferMs < 40);
      CHECK(ps->codecMs > 1 && ps->codecMs < 20);
      if (q->id == 1 && p->id == 2)
        printf("  1 hears 2: SNR %.1f dB, buffer %.1f ms, codec %.1f ms, one way %.1f ms\n", snr,
               ps->bufferMs, ps->codecMs, ps->oneWayMs);
    }
    // the slots nobody has are silent
    for (int k = 3; k < Core::kMaxPeers; k++)
    {
      CHECK(!q->core->slotHealthy(k));
      const float *s = q->core->stem(k, 0);
      CHECK(s[0] == 0 && s[q->block - 1] == 0);
    }
    // 2 s of 5 ms frames, sent once for all three listeners
    printf("  player %u: %ld datagrams up (%ld to one player)\n", q->id, q->uploads, q->unicast_uploads);
    CHECK(q->uploads > 380 && q->uploads < 460);
    CHECK(q->unicast_uploads < 30); // pings and their answers
  }
  CHECK_EQ(n.server_bad, 0);
  CHECK_EQ(n.client_bad, 0);
  for (auto &p : n.players)
  {
    const Core::Totals &t = p->core->totals();
    CHECK(t.accepted > 0);
    CHECK_EQ(t.badSize.load(), 0u);
    CHECK_EQ(t.noSlot.load(), 0u);
    CHECK_EQ(t.filtered.load(), 0u);
    CHECK_EQ(t.badFormat.load(), 0u);
    CHECK_EQ(t.sendTooLarge.load(), 0u);
  }
}

// How late a player hears another: the network, the jitter buffer, the
// codec and the adapters, as the statistics say (here without the round
// trip, which AOO measures on the wall clock).
static void test_glue_delay()
{
  if (!selected("glue_delay")) return;
  for (int jitter_ms : { 20, 50 })
  {
    net n(3);
    player &a = n.add(1);
    player &b = n.add(2, 48000, 96);
    a.up.delay = 0.006;
    a.down.delay = 0.004;
    b.up.delay = 0.007;
    b.down.delay = 0.009;
    bursts bu;
    bu.install(a);
    b.send = false;
    n.start_all();
    b.core->setJitterMs(jitter_ms);
    n.run(1.5);
    start_recording(n);
    n.run(4.0);
    const std::vector<double> d = bu.delays(a, b);
    const std::vector<PeerStats> ps_all = b.core->stats(n.now);
    const PeerStats *ps = stats_of(ps_all, 1);
    CHECK(ps != nullptr);
    if (!ps) continue;
    const double net_ms = 1000 * (a.up.delay + b.down.delay);
    // (AOO's pings measure the round trip on the wall clock, which says
    // nothing on a virtual network: the known network delay instead)
    const double expect = net_ms + ps->oneWayMs - ps->rttMs / 2;
    printf("  jitter buffer %d ms: %zu bursts, delay %.2f ms (spread %.2f), network %.1f + estimate %.2f = %.2f ms\n",
           jitter_ms, d.size(), 1000 * median(d), 1000 * spread(d), net_ms, ps->oneWayMs - ps->rttMs / 2, expect);
    CHECK(d.size() >= 18);
    CHECK(spread(d) < 0.001);
    CHECK(fabs(1000 * median(d) - expect) < 3);
    // the buffer setting plus a few ms of codec and blocking
    const double extra = 1000 * median(d) - net_ms - jitter_ms;
    CHECK(extra > 2 && extra < 15);
    CHECK(fabs(ps->bufferMs - jitter_ms) < 6);
  }
}

// Lost packets come back through resend requests, in time for the buffer;
// the statistics count them.
static void test_glue_loss_resend()
{
  if (!selected("glue_loss_resend")) return;
  net n(5);
  player &a = n.add(1);
  player &b = n.add(2);
  a.up.loss = 0.02;
  a.up.jitter = 0.003;
  b.down.loss = 0.05;
  b.down.jitter = 0.004;
  b.down.dup = 0.01;
  b.up.loss = 0.02; // the resend requests
  b.send = false;
  n.start_all();
  b.core->setJitterMs(40);
  n.run(1.0);
  start_recording(n);
  n.run(8.0);
  const std::vector<PeerStats> ps_all = b.core->stats(n.now);
  const PeerStats *ps = stats_of(ps_all, 1);
  CHECK(ps != nullptr);
  if (!ps) return;
  const double snr = heard_snr(b, a, 6.0);
  printf("  lost %ld up, %ld down; resent %d, dropped %d, underruns %d, loss %.1f%%, SNR %.1f dB\n",
         a.up.lost, b.down.lost, ps->resent, ps->dropped, ps->underruns, ps->lossPct, snr);
  CHECK(ps->resent > 50);
  // (a resent block can be lost again, and its request too)
  CHECK(ps->dropped * 10 < a.up.lost + b.down.lost);
  CHECK(ps->underruns <= 1);
  CHECK(ps->lossPct > 3 && ps->lossPct < 12);
  CHECK(snr > 20);
  CHECK(ps->healthy);
  CHECK(all_finite(b.rec[0]));
}

// When the buffer is too short for a resend, Opus conceals the gaps (PLC):
// the stream goes on, without silent holes.
static void test_glue_loss_concealed()
{
  if (!selected("glue_loss_concealed")) return;
  net n(9);
  player &a = n.add(1);
  player &b = n.add(2);
  a.up.delay = b.down.delay = 0.02; // a 40 ms round trip
  b.down.burst = 0.004;             // about one 20 ms burst a second
  b.down.burst_len = 0.02;
  b.down.loss = 0.01;
  b.send = false;
  n.start_all();
  b.core->setJitterMs(10);
  n.run(1.0);
  start_recording(n);
  n.run(8.0);
  const std::vector<PeerStats> ps_all = b.core->stats(n.now);
  const PeerStats *ps = stats_of(ps_all, 1);
  CHECK(ps != nullptr);
  if (!ps) return;
  const int slot = b.core->slotOf(1);
  const double silent = silent_share(b.rec[slot], b.rate, 0.05);
  // (one fit over seconds would fail on the shifts an underrun makes)
  const double good = good_share(b.rec[slot], a.tone_hz, b.rate, 15);
  printf("  lost %ld; dropped %d, resent %d, underruns %d; silent windows %.1f%%, clean windows %.1f%%\n",
         b.down.lost, ps->dropped, ps->resent, ps->underruns, 100 * silent, 100 * good);
  CHECK(b.down.lost > 50);
  CHECK(ps->dropped > 10);
  CHECK(silent < 0.02);
  CHECK(good > 0.85);
  CHECK(all_finite(b.rec[slot]));
}

// 44.1, 48 and 96 kHz in one room
static void test_glue_sample_rates()
{
  if (!selected("glue_sample_rates")) return;
  net n(13);
  n.add(1, 44100, 100);
  n.add(2, 48000, 128);
  n.add(3, 96000, 256);
  n.start_all();
  n.run(2.0);
  start_recording(n);
  n.run(2.0);
  for (auto &q : n.players)
    for (auto &p : n.players)
    {
      if (p == q) continue;
      const double snr = heard_snr(*q, *p, 1.5);
      printf("  %g Hz hears %g Hz: SNR %.1f dB\n", q->rate, p->rate, snr);
      CHECK(snr > 20);
      CHECK(q->core->slotHealthy(q->core->slotOf(p->id)));
      const std::vector<PeerStats> ps_all = q->core->stats(n.now);
      const PeerStats *ps = stats_of(ps_all, p->id);
      CHECK(ps && ps->underruns == 0 && ps->dropped == 0);
    }
}

// Two audio interfaces 200 ppm apart, for five minutes: dynamic resampling
// keeps the buffer where it was, without dropouts. Without it, the delay
// creeps until the buffer runs dry or over.
static void test_glue_drift()
{
  if (!selected("glue_drift")) return;
#ifdef NDEBUG
  const int last = 0;
  const double minutes_dr = 5;
#else
  // a debug build (sanitizers) runs about 15 times slower: one minute, and
  // no run without dynamic resampling
  const int last = 1;
  const double minutes_dr = 1;
#endif
  for (int dr = 1; dr >= last; dr--)
  {
    net n(17);
    player &a = n.add(1, 48000, 128);
    player &b = n.add(2, 44100, 64);
    a.ppm = 100;
    b.ppm = -100;
    a.up.jitter = b.down.jitter = 0.002;
    bursts bu;
    bu.install(a);
    b.send = false;
    a.core->setDynamicResampling(dr == 1);
    b.core->setDynamicResampling(dr == 1);
    n.start_all();
    const double minutes = dr ? minutes_dr : 2.5;
    n.run(2.0);
    start_recording(n);
    n.run(3.0);
    const double d0 = median(bu.delays(a, b));
    b.recording = false;
    b.clear_recording();
    const std::vector<PeerStats> s0_all = b.core->stats(n.now);
    const PeerStats *s0 = stats_of(s0_all, 1);
    const int dropouts0 = s0 ? s0->underruns + s0->overruns + s0->dropped : 0;
    n.run(minutes * 60 - 8);
    start_recording(n);
    n.run(3.0);
    const double d1 = median(bu.delays(a, b));
    const std::vector<PeerStats> s1_all = b.core->stats(n.now);
    const PeerStats *s1 = stats_of(s1_all, 1);
    CHECK(s1 != nullptr);
    if (!s1) continue;
    const int dropouts = s1->underruns + s1->overruns + s1->dropped - dropouts0;
    printf("  %s, %.1f min: delay %.2f -> %.2f ms; underruns %d, overruns %d, dropped %d, after warm-up %d\n",
           dr ? "dynamic resampling" : "no resampling", minutes, 1000 * d0, 1000 * d1, s1->underruns,
           s1->overruns, s1->dropped, dropouts);
    if (dr)
    {
      CHECK(fabs(d1 - d0) < 0.002);
      CHECK(dropouts <= 2);
      CHECK(s1->healthy);
    }
    else
    {
      // 200 ppm for 2.5 minutes is 30 ms: more than the buffer holds
      CHECK(fabs(d1 - d0) > 0.005 || dropouts > 0);
    }
  }
}

// Nine others for eight slots: the ninth waits for a slot, then gets the
// one a leaver frees, reset (statistics, buffer) for it.
static void test_glue_slot_reuse()
{
  if (!selected("glue_slot_reuse")) return;
  net n(19);
  for (uint32_t id = 1; id <= 10; id++) n.add(id, 48000, 128);
  player &l = *n.find(1);
  n.start_all();
  n.run(2.0);
  CHECK_EQ(l.core->slotOf(10), -1);
  for (uint32_t id = 2; id <= 9; id++) CHECK_EQ(l.core->slotOf(id), (int)id - 2);
  for (int k = 0; k < Core::kMaxPeers; k++) CHECK(l.core->slotHealthy(k));
  const uint64_t noSlot = l.core->totals().noSlot.load();
  CHECK(noSlot > 100); // player 10's packets
  const std::vector<PeerStats> s10_all = l.core->stats(n.now);
  const PeerStats *s10 = stats_of(s10_all, 10);
  CHECK(s10 && s10->slot == -1 && !s10->streaming);

  // player 3 leaves (the server stops copying to it and from it)
  const int slot = l.core->slotOf(3);
  player &p3 = *n.find(3);
  p3.in_room = false;
  p3.core->setStreaming(false);
  n.update_peers();
  CHECK_EQ(l.core->slotOf(3), -1);
  CHECK_EQ(l.core->slotOf(10), slot);
  CHECK(!l.core->slotHealthy(slot));
  const std::vector<PeerStats> fresh_all = l.core->stats(n.now);
  const PeerStats *fresh = stats_of(fresh_all, 10);
  CHECK(fresh && fresh->slot == slot && !fresh->streaming && fresh->resent == 0 && fresh->underruns == 0);
  CHECK(n.run_until([&] { return l.core->slotHealthy(slot); }, 2.0));
  start_recording(n);
  n.run(1.0);
  const std::vector<float> &r = l.rec[slot];
  const double snr10 = tone_snr(r.data(), r.size(), n.find(10)->tone_hz, l.rate);
  const double snr3 = tone_snr(r.data(), r.size(), p3.tone_hz, l.rate);
  printf("  slot %d: player 10 at %.1f dB, player 3 at %.1f dB\n", slot, snr10, snr3);
  CHECK(snr10 > 20);
  CHECK(snr3 < -20);
  for (uint32_t id : { 2, 4, 5, 6, 7, 8, 9 }) CHECK(heard_snr(l, *n.find(id), 0.9) > 20);
  // nobody hears player 3 any more, and player 3 hears nobody
  for (auto &q : n.players)
    if (q->id != 3) CHECK_EQ(q->core->slotOf(3), -1);
  // (player 3's own Core still has its slots: the engine would stop it)
}

// Packets the filter in front of AOO lets through, and what it doesn't.
static std::vector<uint8_t> osc_message(const char *address, std::function<void(osc::OutboundPacketStream &)> args)
{
  char buf[2048];
  osc::OutboundPacketStream msg(buf, sizeof(buf));
  msg << osc::BeginMessage(address);
  args(msg);
  msg << osc::EndMessage;
  return std::vector<uint8_t>((const uint8_t *)msg.Data(), (const uint8_t *)msg.Data() + msg.Size());
}

static std::vector<uint8_t> start_message(const char *codec, int channels, int rate, int block)
{
  return osc_message("/aoo/sink/1/start", [&](osc::OutboundPacketStream &m) {
    const char blob[4] = { 0, 0, 0, 0 };
    m << (osc::int32)1 << "2.0.0" << (osc::int32)7 << (osc::int32)0 << (osc::int32)1 << (osc::int32)channels
      << (osc::int32)rate << (osc::int32)block << codec << osc::Blob(blob, 4);
  });
}

static void test_glue_prefilter()
{
  if (!selected("glue_prefilter")) return;
  Core core;
  core.setSelf(1);
  jamsa_live::Peer peer;
  peer.id = 5;
  core.setPeers({ peer });
  core.setRoomOn(true);
  const int slot = core.slotOf(5);
  CHECK_EQ(slot, 0);
  auto verdict = [&](uint32_t from, const std::vector<uint8_t> &m, int *target = nullptr) {
    int t = -2;
    const Core::Verdict v = core.classify(from, m.data(), m.size(), &t);
    if (target) *target = t;
    return v;
  };
  int target = -2;
  // sizes
  CHECK_EQ(core.classify(5, nullptr, 0, &target), Core::kBadSize);
  CHECK_EQ(verdict(5, std::vector<uint8_t>(1453, 0x80)), Core::kBadSize);
  // binary /data between ID 1 and 1, to the sink or to our source
  std::vector<uint8_t> bin = { 0x80 | kAooMsgTypeSink, kAooBinMsgCmdData, 1, 1, 0, 0, 0, 0 };
  CHECK_EQ(verdict(5, bin, &target), Core::kAccepted);
  CHECK_EQ(target, slot);
  CHECK_EQ(verdict(6, bin), Core::kNoSlot);
  CHECK_EQ(verdict(1, bin), Core::kNoSlot); // ourselves
  CHECK_EQ(verdict(0, bin), Core::kNoSlot);
  bin[0] = 0x80 | kAooMsgTypeSource;
  CHECK_EQ(verdict(5, bin, &target), Core::kAccepted);
  CHECK_EQ(target, -1);
  bin[0] = 0x80 | kAooMsgTypeSink;
  bin[2] = 2; // to sink 2
  CHECK_EQ(verdict(5, bin), Core::kFiltered);
  bin[2] = 1;
  bin[3] = 3; // from source 3
  CHECK_EQ(verdict(5, bin), Core::kFiltered);
  bin[3] = 1;
  bin[1] = 1; // not /data
  CHECK_EQ(verdict(5, bin), Core::kFiltered);
  bin[1] = kAooBinMsgCmdData | 0x80; // the long header (IDs above 255)
  CHECK_EQ(verdict(5, bin), Core::kFiltered);
  bin[1] = kAooBinMsgCmdData;
  bin[0] = 0x80 | 5; // no such type
  CHECK_EQ(verdict(5, bin), Core::kFiltered);
  CHECK_EQ(verdict(5, std::vector<uint8_t>{ 0x80 | kAooMsgTypeSink, 0, 1 }), Core::kBadSize);

  // OSC: the format of a /start
  CHECK_EQ(verdict(5, start_message("opus", 2, 48000, 240), &target), Core::kAccepted);
  CHECK_EQ(target, slot);
  CHECK_EQ(verdict(5, start_message("opus", 1, 48000, 120)), Core::kAccepted);
  CHECK_EQ(verdict(5, start_message("opus", 2, 48000, 480)), Core::kAccepted);
  CHECK_EQ(verdict(5, start_message("pcm", 2, 48000, 240)), Core::kBadFormat);
  CHECK_EQ(verdict(5, start_message("opus", 3, 48000, 240)), Core::kBadFormat);
  CHECK_EQ(verdict(5, start_message("opus", 0, 48000, 240)), Core::kBadFormat);
  CHECK_EQ(verdict(5, start_message("opus", 2, 44100, 240)), Core::kBadFormat);
  CHECK_EQ(verdict(5, start_message("opus", 2, 48000, 960)), Core::kBadFormat);
  CHECK_EQ(verdict(5, start_message("opus", 2, 48000, 60)), Core::kBadFormat);
  CHECK_EQ(verdict(5, osc_message("/aoo/sink/1/start", [](osc::OutboundPacketStream &m) {
                     m << (osc::int32)1 << "2.0.0" << (osc::int32)7;
                   })), Core::kBadFormat);
  CHECK_EQ(verdict(5, osc_message("/aoo/sink/1/start", [](osc::OutboundPacketStream &m) {
                     m << (osc::int32)1 << (osc::int32)2 << (osc::int32)7 << (osc::int32)0 << (osc::int32)1
                       << (osc::int32)2 << (osc::int32)48000 << (osc::int32)240 << "opus";
                   })), Core::kBadFormat);
  // OSC: IDs and kinds
  auto from1 = [](osc::OutboundPacketStream &m) { m << (osc::int32)1 << (osc::int32)0; };
  auto from2 = [](osc::OutboundPacketStream &m) { m << (osc::int32)2 << (osc::int32)0; };
  CHECK_EQ(verdict(5, osc_message("/aoo/sink/1/stop", from1)), Core::kAccepted);
  CHECK_EQ(verdict(5, osc_message("/aoo/sink/1/ping", from1)), Core::kAccepted);
  CHECK_EQ(verdict(5, osc_message("/aoo/sink/1/data", from1)), Core::kAccepted);
  CHECK_EQ(verdict(5, osc_message("/aoo/sink/1/stop", from2)), Core::kFiltered);
  CHECK_EQ(verdict(5, osc_message("/aoo/sink/2/stop", from1)), Core::kFiltered);
  CHECK_EQ(verdict(5, osc_message("/aoo/sink/1/decline", from1)), Core::kFiltered);
  CHECK_EQ(verdict(5, osc_message("/aoo/sink/1/stop", [](osc::OutboundPacketStream &) {})), Core::kFiltered);
  CHECK_EQ(verdict(5, osc_message("/aoo/sink/1/stop", [](osc::OutboundPacketStream &m) { m << "1"; })),
           Core::kFiltered);
  CHECK_EQ(verdict(5, osc_message("/aoo/src/1/start", from1), &target), Core::kAccepted);
  CHECK_EQ(target, -1);
  CHECK_EQ(verdict(5, osc_message("/aoo/src/1/data", from1)), Core::kAccepted);
  CHECK_EQ(verdict(5, osc_message("/aoo/src/1/pong", from1)), Core::kAccepted);
  CHECK_EQ(verdict(5, osc_message("/aoo/src/1/invite", from1)), Core::kFiltered);
  CHECK_EQ(verdict(5, osc_message("/aoo/src/1/uninvite", from1)), Core::kFiltered);
  CHECK_EQ(verdict(5, osc_message("/aoo/src/2/start", from1)), Core::kFiltered);
  CHECK_EQ(verdict(5, osc_message("/aoo/server/1/start", from1)), Core::kFiltered);
  CHECK_EQ(verdict(5, osc_message("/foo", from1)), Core::kFiltered);
  const char bundle[] = "#bundle\0\0\0\0\0\0\0\0\1";
  CHECK_EQ(verdict(5, std::vector<uint8_t>(bundle, bundle + 16)), Core::kFiltered);
  std::vector<uint8_t> cut = osc_message("/aoo/sink/1/stop", from1);
  cut.resize(cut.size() - 3);
  CHECK_EQ(verdict(5, cut), Core::kFiltered);

  // room off: nothing
  core.setRoomOn(false);
  CHECK_EQ(verdict(5, osc_message("/aoo/sink/1/stop", from1)), Core::kFiltered);
  bin[0] = 0x80 | kAooMsgTypeSink;
  CHECK_EQ(verdict(5, bin), Core::kFiltered);

  // handleData counts what it drops
  core.setRoomOn(true);
  const std::vector<uint8_t> pcm = start_message("pcm", 2, 48000, 240);
  CHECK_EQ(core.handleData(5, pcm.data(), pcm.size()), Core::kBadFormat);
  CHECK_EQ(core.handleData(6, bin.data(), bin.size()), Core::kNoSlot);
  CHECK_EQ(core.totals().badFormat.load(), 1u);
  CHECK_EQ(core.totals().noSlot.load(), 1u);
}

// Mangled and random packets into a player that hears two others: nothing
// breaks, and it still hears them.
static void test_glue_fuzz()
{
  if (!selected("glue_fuzz")) return;
  net n(23);
  n.add(1);
  n.add(2);
  n.add(3);
  player &l = *n.find(1);
  std::vector<std::vector<uint8_t>> seen;
  n.copy_filter = [&](uint32_t, uint32_t to, const uint8_t *p, size_t size) {
    if (to == 1 && seen.size() < 3000) seen.push_back(std::vector<uint8_t>(p, p + size));
    return true;
  };
  n.start_all();
  n.run(1.5);
  CHECK(seen.size() > 500);
  std::mt19937 rng(29);
  long fed = 0;
  start_recording(n);
  for (int round = 0; round < 300; round++)
  {
    n.run(0.01);
    for (int i = 0; i < 20; i++)
    {
      std::vector<uint8_t> b;
      const int kind = (int)(rng() % 4);
      if (kind == 0)
      {
        b.resize(rng() % 1500);
        for (auto &x : b) x = (uint8_t)rng();
        if (!b.empty() && (rng() & 1)) b[0] |= 0x80;
      }
      else
      {
        b = seen[rng() % seen.size()];
        const int flips = 1 + (int)(rng() % 8);
        for (int f = 0; f < flips && !b.empty(); f++)
        {
          // mostly in the headers, where the lengths and counts are
          const size_t at = (rng() & 1) ? rng() % std::min<size_t>(b.size(), 40) : rng() % b.size();
          b[at] = (kind == 1) ? (uint8_t)rng() : (uint8_t)(b[at] ^ (1u << (rng() % 8)));
        }
        if (kind == 3) b.resize(rng() % (b.size() + 64), (uint8_t)rng());
      }
      const uint32_t from = (rng() % 8 == 0) ? (uint32_t)(rng() % 12) : 2 + (uint32_t)(rng() % 2);
      l.core->handleData(from, b.data(), b.size());
      fed++;
    }
  }
  const Core::Totals &t = l.core->totals();
  printf("  fed %ld: accepted %llu (with the real ones), bad size %llu, no slot %llu, filtered %llu, bad format %llu\n",
         fed, (unsigned long long)t.accepted.load(), (unsigned long long)t.badSize.load(),
         (unsigned long long)t.noSlot.load(), (unsigned long long)t.filtered.load(),
         (unsigned long long)t.badFormat.load());
  for (int k = 0; k < 2; k++) CHECK(all_finite(l.rec[k]));
  // a quiet second, and both are back
  n.run(2.0);
  CHECK(l.core->slotHealthy(l.core->slotOf(2)));
  CHECK(l.core->slotHealthy(l.core->slotOf(3)));
  start_recording(n);
  n.run(1.0);
  CHECK(heard_snr(l, *n.find(2), 0.9) > 20);
  CHECK(heard_snr(l, *n.find(3), 0.9) > 20);
}

// The room's Live flag off: slots go quiet at once, nothing reaches AOO,
// the source stops; on again: back within a second or two.
static void test_glue_room_off()
{
  if (!selected("glue_room_off")) return;
  net n(31);
  player &a = n.add(1);
  player &b = n.add(2);
  n.start_all();
  n.run(2.0);
  const int slot = b.core->slotOf(1);
  CHECK(b.core->slotHealthy(slot));
  b.core->setRoomOn(false);
  CHECK(!b.core->slotHealthy(slot));
  const uint64_t filtered = b.core->totals().filtered.load();
  n.run(0.1);
  CHECK(b.core->totals().filtered.load() > filtered + 5); // a's packets
  start_recording(n);
  n.run(0.1);
  CHECK(!b.core->slotHealthy(slot));
  double sum = 0;
  for (float v : b.rec[slot]) sum += fabs(v);
  CHECK(sum == 0);
  // a's room goes off too: its stream stops
  a.core->setRoomOn(false);
  a.core->setStreaming(false);
  n.run(0.2);
  const long uploads = a.uploads;
  n.run(1.0);
  CHECK(a.uploads - uploads < 5);
  b.core->setStreaming(false);
  // on again
  for (auto &p : n.players)
  {
    p->core->setRoomOn(true);
    p->core->setStreaming(true);
  }
  CHECK(n.run_until([&] { return b.core->slotHealthy(slot) && a.core->slotHealthy(a.core->slotOf(2)); }, 2.5));
  start_recording(n);
  n.run(1.0);
  CHECK(heard_snr(b, a, 0.9) > 20);
  CHECK(heard_snr(a, b, 0.9) > 20);
}

// The host stops calling for a second (processing suspended): both ends
// get over it.
static void test_glue_gap()
{
  if (!selected("glue_gap")) return;
  for (int who = 1; who <= 2; who++)
  {
    net n(37);
    n.add(1);
    n.add(2);
    player &a = *n.find(1);
    player &b = *n.find(2);
    n.start_all();
    n.run(2.0);
    player &stopped = who == 1 ? a : b;
    stopped.host_running = false;
    n.run(1.0);
    stopped.host_running = true;
    const bool back = n.run_until([&] { return b.core->slotHealthy(b.core->slotOf(1)) && a.core->slotHealthy(a.core->slotOf(2)); }, 3.0);
    printf("  %s stopped for 1 s: %s\n", who == 1 ? "sender" : "listener", back ? "back" : "NOT back");
    CHECK(back);
    start_recording(n);
    n.run(1.0);
    CHECK(heard_snr(b, a, 0.9) > 20);
    CHECK(heard_snr(a, b, 0.9) > 20);
  }
}

// The room's bitrate changes the packets' size at once; its frame size
// restarts the stream with the new format.
static void test_glue_bitrate_frame()
{
  if (!selected("glue_bitrate_frame")) return;
  net n(41);
  player &a = n.add(1);
  player &b = n.add(2);
  b.send = false;
  n.start_all();
  n.run(1.0);
  auto measure = [&](double seconds, double *per_second) {
    const long up = a.uploads, bytes = a.upload_bytes;
    n.run(seconds);
    *per_second = (a.uploads - up) / seconds;
    return (double)(a.upload_bytes - bytes) / std::max(1L, a.uploads - up);
  };
  double rate = 0;
  a.core->configure(64, 240);
  n.run(0.2);
  const double small = measure(1.0, &rate);
  a.core->configure(256, 240);
  n.run(0.2);
  const double large = measure(1.0, &rate);
  printf("  64 kbps: %.0f bytes a datagram, 256 kbps: %.0f, at %.0f a second\n", small, large, rate);
  // the Opus part: 40 -> 160 bytes (a tone needs no more, at any rate)
  CHECK(large - small > 100);
  CHECK(rate > 190 && rate < 215);
  CHECK(b.core->slotHealthy(0)); // no restart
  for (int frame : { 120, 480 })
  {
    a.core->configure(128, frame);
    n.run(0.5);
    CHECK(n.run_until([&] { return b.core->slotHealthy(0); }, 2.5));
    measure(1.0, &rate);
    printf("  %d-sample frames: %.0f datagrams a second\n", frame, rate);
    CHECK(fabs(rate - 48000.0 / frame) < 0.08 * 48000.0 / frame);
    start_recording(n);
    n.run(1.0);
    CHECK(heard_snr(b, a, 0.9) > 20);
  }
}

int main(int argc, char **argv)
{
  printf("live_unit_test\n");
  if (argc > 1) g_only = argv[1];
  aoo_initialize(nullptr);

  test_opus_coupled_stereo();
  test_opus_bitrate();
  test_opus_bitrate_source();

  test_broadcast_three_sinks();
  test_broadcast_resend();
  test_broadcast_late_sink();
  test_broadcast_toggle();

  test_remove_source();

  test_glue_wire();
  test_glue_room_of_four();
  test_glue_delay();
  test_glue_loss_resend();
  test_glue_loss_concealed();
  test_glue_sample_rates();
  test_glue_drift();
  test_glue_slot_reuse();
  test_glue_prefilter();
  test_glue_fuzz();
  test_glue_room_off();
  test_glue_gap();
  test_glue_bitrate_frame();

  return test_summary("live_unit_test");
}
