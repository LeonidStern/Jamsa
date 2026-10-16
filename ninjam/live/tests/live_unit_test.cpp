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
  - AooSink::removeSource(), for a sink that gets a new peer.

  Run with a test name to run only the tests whose names start with it.

*/

#include "detail.hpp" // aoo::find_codec()

#include "live_harness.h"
#include "testutil.h"

#include "opus.h"

#include <math.h>
#include <string.h>

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

  return test_summary("live_unit_test");
}
