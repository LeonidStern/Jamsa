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
    bitrate given with the format, and OPUS_GET_BITRATE.

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

int main(int argc, char **argv)
{
  printf("live_unit_test\n");
  if (argc > 1) g_only = argv[1];
  aoo_initialize(nullptr);

  test_opus_coupled_stereo();
  test_opus_bitrate();
  test_opus_bitrate_source();

  return test_summary("live_unit_test");
}
