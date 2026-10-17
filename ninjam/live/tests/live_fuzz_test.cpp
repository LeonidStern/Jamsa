/*
    Jamsa Live - live_fuzz_test.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Hostile and mangled packets into AOO's sink and source: the side of Live
  mode that the other players feed, through our server. It counts most when
  built with AddressSanitizer and UndefinedBehaviorSanitizer; the checks
  here are that nothing crashes or reads out of bounds, that the sinks'
  output stays finite, and that a new stream plays normally afterwards.

    live_fuzz_test [iterations] [seed] [test]

  (test: one of the names in main(), or "fuzz")

  First one test per hardening fix in our AOO copy (README.md, "Hardening"),
  each with a hand-made packet. Then the fuzzing: packets captured from a
  clean run (binary and OSC, with some loss so that resend requests show
  up too) and mutated; packets built field by field with values around
  every limit; and random bytes. They go to a source and three sinks
  (48 kHz with dynamic resampling, 48 kHz at the stream's block size,
  44.1 kHz) from the real peers' addresses and from a few strangers', while
  the stream keeps running.

*/

#include "binmsg.hpp"
#include "common/utils.hpp"
#include "osc/OscOutboundPacketStream.h"

#include "live_harness.h"
#include "testutil.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace live_harness;

typedef std::vector<AooByte> bytes;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// AOO logs at Error level by default (aoo_config.h): nothing a peer sends
// should make it log at that level, or a hostile peer could flood the log.
static std::atomic<long> g_error_logs{0};

static void AOO_CALL count_log(AooLogLevel level, const AooChar *msg)
{
  if (level == kAooLogLevelError)
  {
    g_error_logs++;
    fprintf(stderr, "  AOO error: %s\n", msg);
  }
}

static std::vector<sink_options> three_sinks()
{
  sink_options a, b, c;
  a.dynamic_resampling = true;
  b.blocksize = 240; // the stream's block size: no resampling or reblocking
  c.samplerate = 44100;
  return { a, b, c };
}

static std::string address(bool to_sink, const char *command, AooId id = 1)
{
  return std::string(kAooMsgDomain) + (to_sink ? kAooMsgSink : kAooMsgSource) + "/"
    + std::to_string(id) + command;
}

static bytes osc_bytes(const osc::OutboundPacketStream &m)
{
  return bytes((const AooByte *)m.Data(), (const AooByte *)m.Data() + m.Size());
}

// the fields of a binary /data message, written as given (see write_bin_data()
// in source.cpp); 'size' is the size field, the payload can differ
struct bin_data
{
  int32_t stream_id = 0, seq = 0;
  uint8_t channel = 0, flags = 0;
  uint16_t size = 0;
  uint32_t total = 0;
  uint16_t nframes = 1, frame = 0;
  uint32_t msgsize = 0;
  double sr = 48000;
  uint64_t tt = 0;
  bytes payload;
};

static bytes binary_data(const bin_data &d, AooId to = 1, AooId from = 1)
{
  bytes b(64 + d.payload.size());
  AooByte *start = b.data();
  const int n = aoo::binmsg_write_header(start, b.size(), kAooMsgTypeSink, kAooBinMsgCmdData, to, from);
  AooByte *it = start + n;
  aoo::write_bytes<int32_t>(d.stream_id, it);
  aoo::write_bytes<int32_t>(d.seq, it);
  aoo::write_bytes<uint8_t>(d.channel, it);
  aoo::write_bytes<uint8_t>(d.flags, it);
  aoo::write_bytes<uint16_t>(d.size, it);
  if (d.flags & kAooBinMsgDataFrames)
  {
    aoo::write_bytes<uint32_t>(d.total, it);
    aoo::write_bytes<uint16_t>(d.nframes, it);
    aoo::write_bytes<uint16_t>(d.frame, it);
  }
  if (d.flags & kAooBinMsgDataStreamMessage) aoo::write_bytes<uint32_t>(d.msgsize, it);
  if (d.flags & kAooBinMsgDataSampleRate) aoo::write_bytes<double>(d.sr, it);
  if (d.flags & kAooBinMsgDataTimeStamp) aoo::write_bytes<uint64_t>(d.tt, it);
  if (!d.payload.empty()) memcpy(it, d.payload.data(), d.payload.size());
  b.resize((it - start) + d.payload.size());
  return b;
}

// a single-frame block as a real source sends it
static bin_data simple_block(int32_t stream_id, int32_t seq, const bytes &payload)
{
  bin_data d;
  d.stream_id = stream_id;
  d.seq = seq;
  d.size = (uint16_t)payload.size();
  d.payload = payload;
  return d;
}

// the /start a real source sends, with every field open to change
struct start_fields
{
  AooId source_id = 1;
  std::string version = aoo_getVersionString();
  int32_t stream_id = 1000, seq_start = 0, format_id = 1000;
  int32_t channels = 2, samplerate = 48000, blocksize = 240;
  std::string codec = "opus";
  int32_t application = OPUS_APPLICATION_RESTRICTED_LOWDELAY;
  int32_t latency = 0, codec_delay = 120, offset = 0;
};

static bytes start_message(const start_fields &f, AooId sink_id = 1)
{
  char buf[1024];
  osc::OutboundPacketStream m(buf, sizeof(buf));
  AooByte ext[4];
  aoo::to_bytes<int32_t>(f.application, ext);
  m << osc::BeginMessage(address(true, kAooMsgStart, sink_id).c_str()) << f.source_id
    << f.version.c_str() << f.stream_id << f.seq_start << f.format_id << f.channels
    << f.samplerate << f.blocksize << f.codec.c_str() << osc::Blob(ext, 4)
    << osc::TimeTag(ntp_from_seconds(1)) << f.latency << f.codec_delay
    << osc::Nil << osc::Nil << f.offset << osc::EndMessage;
  return osc_bytes(m);
}

// what the source currently streams, from its binary /data packets
struct stream_watch
{
  int32_t stream_id = -1, seq = 0;
  bytes last_payload;

  void observe(const datagram &d)
  {
    if (d.from != 0 || !aoo::binmsg_check(d.bytes.data(), d.bytes.size())) return;
    if (aoo::binmsg_cmd(d.bytes.data(), d.bytes.size()) != kAooBinMsgCmdData) return;
    const int n = aoo::binmsg_headersize(d.bytes.data(), d.bytes.size());
    if (n <= 0 || d.bytes.size() < (size_t)n + 12) return;
    const AooByte *p = d.bytes.data() + n;
    stream_id = aoo::from_bytes<int32_t>(p);
    seq = aoo::from_bytes<int32_t>(p + 4);
    const uint8_t flags = p[9];
    if (flags == 0) last_payload.assign(p + 12, d.bytes.data() + d.bytes.size());
  }
};

// run until every sink plays the stream, at most 'seconds'
static bool run_until_all_active(session &s, double seconds)
{
  for (size_t k = 1; k < s.nodes.size(); k++) s.nodes[k].stream_active = 0;
  return s.run_until([&]() {
    for (size_t k = 1; k < s.nodes.size(); k++)
      if (!s.nodes[k].stream_active) return false;
    return true;
  }, seconds);
}

// the RMS of each sink's output over 'seconds'
static std::vector<double> output_rms(session &s, double seconds)
{
  std::vector<double> sum(s.nodes.size(), 0);
  std::vector<long> count(s.nodes.size(), 0);
  const double end = s.now + seconds;
  while (s.now < end)
  {
    const int k = s.tick();
    if (k == 0) continue;
    const double r = s.block_rms(k);
    sum[k] += r * r;
    count[k]++;
  }
  std::vector<double> rms;
  for (size_t k = 1; k < s.nodes.size(); k++)
    rms.push_back(count[k] ? sqrt(sum[k] / count[k]) : 0);
  return rms;
}

// a session that is streaming to all three sinks, and what it streams
struct live_session
{
  session s;
  stream_watch watch;

  live_session() : s(source_options(), three_sinks())
  {
    s.filter = [this](datagram &d) { watch.observe(d); return true; };
    s.start();
    CHECK(run_until_all_active(s, 2.0));
    s.run(0.3);
  }
};

// a sine at 0.5 has an RMS of 0.35
static bool sounds_right(double rms) { return rms > 0.25 && rms < 0.45; }

// ---------------------------------------------------------------------------
// One test per hardening fix
// ---------------------------------------------------------------------------

// aoo_parsePattern() read the ID with sscanf(), past the end of a message
// that ends in digits (no NUL), and "%d" overflow is undefined.
static void test_parse_pattern()
{
  AooMsgType type;
  AooId id = -1;
  AooInt32 offset = 0;
  const char *exact = "/aoo/sink/12";
  CHECK_EQ(aoo_parsePattern((const AooByte *)bytes(exact, exact + strlen(exact)).data(),
    (AooInt32)strlen(exact), &type, &id, &offset), kAooOk);
  {
    // the same, in a heap block of exactly its size
    bytes b(exact, exact + strlen(exact));
    CHECK_EQ(aoo_parsePattern(b.data(), (AooInt32)b.size(), &type, &id, &offset), kAooOk);
    CHECK_EQ(id, 12);
    CHECK_EQ(offset, 12);
    CHECK(type == kAooMsgTypeSink);
  }
  const char *bad[] = { "/aoo/sink/99999999999/data", "/aoo/sink/-1/data", "/aoo/sink/",
    "/aoo/sink", "/aoo/sinks/1", "/aoo/src/x", "/aoo/net/1" };
  for (const char *p : bad)
  {
    bytes b(p, p + strlen(p));
    CHECK_EQ(aoo_parsePattern(b.data(), (AooInt32)b.size(), &type, &id, &offset), kAooErrorBadArgument);
  }
  // a large binary header (12 bytes) cut short
  bytes b(12);
  aoo::binmsg_write_header(b.data(), b.size(), kAooMsgTypeSink, kAooBinMsgCmdData, 1000, 1000);
  b.resize(8);
  CHECK_EQ(aoo_parsePattern(b.data(), (AooInt32)b.size(), &type, &id, &offset), kAooErrorBadArgument);
}

// Datagrams larger than any source or sink sends are dropped up front, and
// malformed OSC.
static void test_packet_size()
{
  session s(source_options(), { sink_options() });
  bytes big = start_message(start_fields());
  big.resize(AOO_MAX_PACKET_SIZE + 1, 0);
  CHECK_EQ(s.inject(0, 1, big.data(), big.size()), kAooErrorBadFormat);
  CHECK_EQ(s.inject(1, 0, big.data(), big.size()), kAooErrorBadFormat);
  CHECK_EQ(s.inject(0, 1, big.data(), 0), kAooErrorBadFormat);

  // OSC with no type tags (just ",") and bytes after them: oscpack took the
  // message's last byte for the type tag string, so an 'i' there made the
  // first argument read from a null pointer
  for (int to = 0; to < 2; to++)
  {
    std::string m = address(to == 1, kAooMsgData);
    m.resize((m.size() + 4) & ~(size_t)3, '\0');
    m += std::string(",\0\0\0", 4);
    m += "xxxi";
    CHECK_EQ(s.inject(1 - to, to, (const AooByte *)m.data(), m.size()), kAooErrorBadFormat);
  }
}

// Every field of a /data message that sizes memory or indexes an array is
// checked before the packet reaches a source; a bad one is dropped.
static void test_data_fields()
{
  live_session ls;
  session &s = ls.s;
  const int32_t sid = ls.watch.stream_id;
  CHECK(sid >= 0);
  const bytes payload(50, 0x55);
  std::vector<bin_data> bad;
  bin_data d = simple_block(sid, ls.watch.seq + 5, payload);
  d.flags = kAooBinMsgDataFrames;
  d.total = 100; d.nframes = 2; d.frame = 2; bad.push_back(d);          // frame past the end
  d.nframes = 0; d.frame = 0; bad.push_back(d);                          // no frames, but data
  d.nframes = 257; bad.push_back(d);                                     // too many frames
  d.nframes = 2; d.total = AOO_MAX_BLOCK_DATA_SIZE + 1; bad.push_back(d); // block too large
  d.total = 60000; bad.push_back(d); // (64 KB were allowed: that much of the audio thread's stack)
  d.total = 40; bad.push_back(d);                                        // frame larger than block
  d = simple_block(sid, ls.watch.seq + 5, payload);
  d.flags = kAooBinMsgDataStreamMessage;
  d.msgsize = 3; bad.push_back(d);                                       // not a count and messages
  d.msgsize = 52; bad.push_back(d);                                      // larger than the block
  d = simple_block(sid, ls.watch.seq + 5, payload);
  d.flags = kAooBinMsgDataSampleRate;
  d.sr = std::numeric_limits<double>::quiet_NaN(); bad.push_back(d);
  d.sr = -48000; bad.push_back(d);
  d.sr = std::numeric_limits<double>::infinity(); bad.push_back(d);
  d = simple_block(-5, ls.watch.seq + 5, payload); bad.push_back(d);     // negative stream ID
  d = simple_block(sid, -7, payload); bad.push_back(d);                  // negative sequence
  for (size_t i = 0; i < bad.size(); i++)
  {
    const bytes b = binary_data(bad[i]);
    const AooError err = s.inject(0, 1, b.data(), b.size());
    if (err != kAooErrorBadFormat) printf("  bad /data case %zu: error %d\n", i, (int)err);
    CHECK_EQ(err, kAooErrorBadFormat);
  }

  // the time stamp flag on a packet that ends before the time stamp: read
  // past the end before the size check
  d = simple_block(sid, ls.watch.seq + 5, bytes());
  d.flags = kAooBinMsgDataTimeStamp;
  bytes b = binary_data(d);
  b.resize(b.size() - 8);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooErrorBadFormat);

  // OSC: a negative channel onset indexed the sink's output channels
  // from before the start (a write anywhere, on the audio thread)
  {
    char buf[512];
    osc::OutboundPacketStream m(buf, sizeof(buf));
    m << osc::BeginMessage(address(true, kAooMsgData).c_str()) << (int32_t)1 << sid
      << (int32_t)(ls.watch.seq + 5) << osc::Nil << osc::Nil << (int32_t)-3 << (int32_t)payload.size()
      << osc::Nil << osc::Nil << osc::Nil << osc::Blob(payload.data(), (osc::osc_bundle_element_size_t)payload.size())
      << osc::EndMessage;
    const bytes o = osc_bytes(m);
    CHECK_EQ(s.inject(0, 1, o.data(), o.size()), kAooErrorBadFormat);
  }
  // OSC: a blob that claims more bytes than the message has. oscpack built
  // the exception for it but didn't throw it, and the sink copied the
  // claimed size (an out-of-bounds read of up to 2 GB).
  {
    char buf[512];
    osc::OutboundPacketStream m(buf, sizeof(buf));
    m << osc::BeginMessage(address(true, kAooMsgData).c_str()) << (int32_t)1 << sid
      << (int32_t)(ls.watch.seq + 5) << osc::Nil << osc::Nil << (int32_t)0 << (int32_t)payload.size()
      << osc::Nil << osc::Nil << osc::Nil << osc::Blob(payload.data(), (osc::osc_bundle_element_size_t)payload.size())
      << osc::EndMessage;
    const bytes good = osc_bytes(m);
    // the blob is last: its size, then 50 bytes and 2 of padding
    const size_t at = good.size() - 52 - 4;
    CHECK_EQ(aoo::from_bytes<int32_t>(good.data() + at), (int32_t)payload.size());
    const int32_t sizes[] = { 53, 85, 5000, INT_MAX, -1, -4 };
    for (int32_t n : sizes)
    {
      bytes o = good;
      aoo::to_bytes<int32_t>(n, o.data() + at);
      CHECK_EQ(s.inject(0, 1, o.data(), o.size()), kAooErrorBadFormat);
    }
    // (and the message as built is fine)
    CHECK_EQ(s.inject(0, 1, good.data(), good.size()), kAooOk);
  }
  // a sample rate far off is replaced by the nominal one, not rejected
  d = simple_block(sid, ls.watch.seq + 6, payload);
  d.flags = kAooBinMsgDataSampleRate;
  d.sr = 1e300;
  b = binary_data(d);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooOk);

  const std::vector<double> rms = output_rms(s, 1.0);
  CHECK(s.outputs_finite);
  for (double r : rms) CHECK(sounds_right(r));
}

// Blocks whose frames don't add up: copied into a stack buffer of the
// block's size without a bound (a stack overflow on the audio thread), and
// frames of one block that claim different frame counts.
static void test_frames()
{
  live_session ls;
  session &s = ls.s;
  const int32_t sid = ls.watch.stream_id;
  const int dropped = s.nodes[1].dropped;

  // two frames of 30 bytes for a block of 40
  bin_data d = simple_block(sid, ls.watch.seq + 8, bytes(30, 0x11));
  d.flags = kAooBinMsgDataFrames;
  d.total = 40; d.nframes = 2; d.frame = 0;
  bytes b = binary_data(d);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooOk);
  d.frame = 1;
  b = binary_data(d);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooOk);

  // frame 0 of a 2-frame block, then frame 6 of "7 frames" for the same block
  d = simple_block(sid, ls.watch.seq + 9, bytes(20, 0x22));
  d.flags = kAooBinMsgDataFrames;
  d.total = 40; d.nframes = 2; d.frame = 0;
  b = binary_data(d);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooOk);
  d.total = 140; d.nframes = 7; d.frame = 6;
  b = binary_data(d);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooOk); // accepted here, dropped on the audio thread

  s.run(0.5);
  CHECK(s.nodes[1].dropped > dropped); // the bad block and the real one it displaced
  const std::vector<double> rms = output_rms(s, 1.0);
  CHECK(s.outputs_finite);
  for (double r : rms) CHECK(sounds_right(r));
}

// One block with a sequence far ahead of the stream (a bit flipped in it, a
// peer's forged packet) was an overrun: the sink threw its buffer away and
// took the block for the newest, so every real block after it was
// "outdated" until buffering gave up after four times the latency, with an
// underrun. Such a block is now dropped unless the next new block follows
// it, as when a stream really jumps: then the stream moves, and back.
static void test_sequence_jump()
{
  live_session ls;
  session &s = ls.s;
  // each sink's stream (not in broadcast mode: each has its own stream ID)
  stream_watch watch[4];
  s.filter = [&](datagram &d) {
    if (d.to >= 1 && d.to <= 3) watch[d.to].observe(d);
    return true;
  };
  s.run(0.1);
  int overruns[4], underruns[4];
  for (int k = 1; k <= 3; k++)
  {
    overruns[k] = s.nodes[k].events[kAooEventBufferOverrun];
    underruns[k] = s.nodes[k].events[kAooEventBufferUnderrun];
  }
  // one at a time (the buffers hold 20 blocks)
  const int32_t jumps[] = { 1 << 24, 1 << 10, 40 };
  for (int32_t jump : jumps)
  {
    for (int k = 1; k <= 3; k++)
    {
      const int32_t seq = watch[k].seq + jump;
      const bytes d = binary_data(simple_block(watch[k].stream_id, seq, bytes(40, 0x44)));
      CHECK_EQ(s.inject(0, k, d.data(), d.size()), kAooOk);
    }
    s.run(0.1);
  }
  s.run(0.5);
  for (int k = 1; k <= 3; k++)
  {
    CHECK_EQ(s.nodes[k].events[kAooEventBufferOverrun], overruns[k]);
    CHECK_EQ(s.nodes[k].events[kAooEventBufferUnderrun], underruns[k]);
  }
  // two blocks in a row: the stream moves there, then back with the next
  // two real blocks, without waiting for buffering to give up
  for (int i = 0; i < 2; i++)
  {
    for (int k = 1; k <= 3; k++)
    {
      const int32_t seq = watch[k].seq + (1 << 20) + i;
      const bytes d = binary_data(simple_block(watch[k].stream_id, seq, bytes(40, 0x44)));
      CHECK_EQ(s.inject(0, k, d.data(), d.size()), kAooOk);
    }
  }
  s.run(0.5);
  for (int k = 1; k <= 3; k++)
  {
    CHECK_EQ(s.nodes[k].events[kAooEventBufferOverrun], overruns[k] + 2);
    CHECK_EQ(s.nodes[k].events[kAooEventBufferUnderrun], underruns[k]);
  }
  const std::vector<double> rms = output_rms(s, 1.0);
  CHECK(s.outputs_finite);
  for (double r : rms) CHECK(sounds_right(r));
}

// A /start with seq_start 0, then /data with a sequence far ahead: the gap
// to the newest block (-1 after the /start) was taken in 32 bits, so
// INT32_MAX overflowed to a negative count, the overrun check let it
// through, and the sink filled the gap with 2^31 placeholder blocks on the
// audio thread (6 s per datagram in a release build; an assert in a debug
// one). The fuzzer doesn't find this pair by itself. (A block that far
// ahead now moves the stream only when the next one follows it, see
// sequence_jump, so each comes after the block before it.)
static void test_sequence_gap()
{
  live_session ls;
  session &s = ls.s;
  const int32_t seqs[] = { INT32_MAX, INT32_MAX - 1, 0x40000000, 1 << 20 };
  int32_t stream = 7000;
  double worst = 0;
  for (int32_t seq : seqs)
  {
    for (int k = 1; k <= 3; k++)
    {
      start_fields st;
      st.stream_id = stream;
      st.seq_start = 0;
      const bytes b = start_message(st);
      CHECK_EQ(s.inject(0, k, b.data(), b.size()), kAooOk);
      for (int32_t q : { seq - 1, seq })
      {
        const bytes d = binary_data(simple_block(stream, q, bytes(40, 0x44)));
        CHECK_EQ(s.inject(0, k, d.data(), d.size()), kAooOk);
      }
    }
    stream++;
    // the sinks' next process() takes the block
    const auto t0 = std::chrono::steady_clock::now();
    s.run(0.01);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("  /data %d and the one before, after /start 0: 10 ms of the session took %.2f ms\n", (int)seq, ms);
    worst = std::max(worst, ms);
  }
  CHECK(worst < 500);
  // the real stream comes back: its /data asks the source for /start again
  CHECK(run_until_all_active(s, 3.0));
  s.run(0.5);
  const std::vector<double> rms = output_rms(s, 1.0);
  CHECK(s.outputs_finite);
  for (double r : rms) CHECK(sounds_right(r));
}

// Stream messages in a block: their headers were read without a bound, a
// message of type 0xFFFF (the sink's own stream time type) was later read
// as a time stamp it didn't have, a sink without a message handler called
// a null pointer, and a source could keep any amount scheduled.
static void test_stream_messages()
{
  live_session ls;
  session &s = ls.s;
  s.message_handler = false;
  const int32_t sid = ls.watch.stream_id;
  for (int k = 0; k < 40; k++)
  {
    // a count of 1000 messages, then real-looking ones
    bytes p;
    AooByte word[4];
    aoo::to_bytes<int32_t>(k == 0 ? 1000 : 14, word);
    p.insert(p.end(), word, word + 4);
    for (int m = 0; m < 14; m++)
    {
      AooByte h[8];
      aoo::to_bytes<uint16_t>((uint16_t)(m * 997), h);          // offset in samples
      aoo::to_bytes<uint16_t>(0, h + 2);                        // channel
      aoo::to_bytes<uint16_t>(m % 3 == 0 ? 0xFFFF : 1, h + 4);  // type
      aoo::to_bytes<uint16_t>(m % 3 == 0 ? 0 : 248, h + 6);     // size
      p.insert(p.end(), h, h + 8);
      if (m % 3) p.insert(p.end(), 248, (AooByte)m);
    }
    const uint32_t msgsize = (uint32_t)p.size();
    p.insert(p.end(), 40, 0x33); // "audio"
    bin_data d = simple_block(sid, ls.watch.seq + 10 + k, p);
    d.flags = kAooBinMsgDataStreamMessage;
    d.msgsize = msgsize;
    const bytes b = binary_data(d);
    CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooOk);
  }
  s.run(1.0);
  CHECK(s.outputs_finite);
  CHECK_EQ(s.nodes[1].stream_messages, 0L);
  const std::vector<double> rms = output_rms(s, 1.0);
  for (double r : rms) CHECK(sounds_right(r));
}

// /start formats outside what Opus or the stack can take are rejected (the
// codec used to "fix" them, and the block size and channel count size
// alloca() buffers on the audio thread). A good format, a bad one, then the
// good one's format ID again ran with no format and no decoder (a null
// pointer dereference from the network).
static void test_start_formats()
{
  live_session ls;
  session &s = ls.s;
  const int active = s.nodes[1].stream_active;
  struct { int32_t channels, samplerate, blocksize; } bad[] = {
    { 255, 48000, 240 }, { 0, 48000, 240 }, { -2, 48000, 240 }, { 9, 48000, 240 },
    { 2, 44100, 240 }, { 2, 0, 240 }, { 2, -48000, 240 }, { 2, 96000, 240 },
    { 2, 48000, 100000 }, { 2, 48000, 0 }, { 2, 48000, 119 }, { 2, 48000, 2881 } };
  int32_t stream = 5000;
  for (auto &f : bad)
  {
    start_fields st;
    st.stream_id = stream++;
    st.format_id = stream;
    st.channels = f.channels;
    st.samplerate = f.samplerate;
    st.blocksize = f.blocksize;
    const bytes b = start_message(st);
    CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooErrorBadFormat);
    s.run(0.01);
  }
  start_fields st;
  st.stream_id = stream++;
  st.codec = "pcm";
  bytes b = start_message(st);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooErrorNotImplemented);
  st.stream_id = stream++;
  st.codec = "opus";
  st.version = "99999999999999999999.1";
  b = start_message(st);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooErrorVersionNotSupported);
  st.version = "2";
  b = start_message(st);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooErrorBadFormat);

  // good format 7, bad format 8, then format 7 again
  st = start_fields();
  st.stream_id = stream++;
  st.format_id = 7;
  b = start_message(st);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooOk);
  s.run(0.01);
  st.stream_id = stream++;
  st.format_id = 8;
  st.channels = 255;
  b = start_message(st);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooErrorBadFormat);
  s.run(0.01);
  st.stream_id = stream++;
  st.format_id = 7;
  st.channels = 2;
  st.latency = -100000; st.codec_delay = -5; st.offset = INT_MIN;
  b = start_message(st);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooOk);
  s.run(0.05);
  // latencies that overflow once added up and resampled (the sink buffers
  // this stream with packet loss concealment, which reports the latencies)
  st.stream_id = stream++;
  st.latency = INT_MAX; st.codec_delay = INT_MAX; st.offset = INT_MAX;
  b = start_message(st);
  CHECK_EQ(s.inject(0, 1, b.data(), b.size()), kAooOk);
  s.run(0.05);
  CHECK(s.outputs_finite);

  // the real stream comes back: its /data asks the source for /start again
  CHECK(s.run_until([&]() { return s.nodes[1].stream_active > active; }, 3.0));
  const std::vector<double> rms = output_rms(s, 1.0);
  CHECK(s.outputs_finite);
  for (double r : rms) CHECK(sounds_right(r));
}

// A /start of the stream a sink plays, replayed with a bit flipped in its
// codec delay or its sample offset (after a /start of another stream, so
// that it isn't redundant; live_unit_test's glue_fuzz): both put off the
// stream's "active" event, by up to 10 s each, so the sink played for that
// long without saying so. Now by 100 ms at most.
static void test_start_delays()
{
  live_session ls;
  session &s = ls.s;
  // each sink's stream (not in broadcast mode: each has its own stream ID)
  stream_watch watch[4];
  s.filter = [&](datagram &d) {
    if (d.to >= 1 && d.to <= 3) watch[d.to].observe(d);
    return true;
  };
  s.run(0.1);
  int active[4];
  for (int k = 1; k <= 3; k++)
  {
    active[k] = s.nodes[k].stream_active;
    start_fields other;
    other.stream_id = 8000 + k;
    bytes b = start_message(other);
    CHECK_EQ(s.inject(0, k, b.data(), b.size()), kAooOk);
    start_fields st;
    st.stream_id = watch[k].stream_id;
    st.seq_start = watch[k].seq;
    if (k != 2) st.offset = 1 << 27;
    if (k != 1) st.codec_delay = 120 | (1 << 26);
    b = start_message(st);
    CHECK_EQ(s.inject(0, k, b.data(), b.size()), kAooOk);
  }
  // buffering (50 ms), then active
  CHECK(s.run_until([&]() {
    for (int k = 1; k <= 3; k++)
      if (s.nodes[k].stream_active == active[k]) return false;
    return true;
  }, 0.5));
  const std::vector<double> rms = output_rms(s, 1.0);
  CHECK(s.outputs_finite);
  for (double r : rms) CHECK(sounds_right(r));
}

static bytes hex_bytes(const char *hex)
{
  bytes b;
  for (; hex[0] && hex[1]; hex += 2) b.push_back((AooByte)strtoul(std::string(hex, 2).c_str(), nullptr, 16));
  return b;
}

// Opus' float decoder doesn't saturate: an Opus packet of the test tone's
// stream with a few bits flipped (from live_unit_test's glue_fuzz) decodes
// into samples of 65000, and the concealment and the next packets carry
// that on. The sinks played it as it was; now what they decode is clamped
// to +/-2.
static void test_loud_packet()
{
  live_session ls;
  session &s = ls.s;
  const bytes loud = hex_bytes(
    "ecb2f9ed07be6f683cc9173cfceb2eecf3d6e1b1d43dac93d619a7e13cc884c70b32a4ec0958666adecad7dcc9"
    "2dae1df6ccc824074997819a60ab76d018bdc310adf7235c0a8555a3d609e7bafd0bdfb9");
  CHECK_EQ(loud.size(), (size_t)81);
  // the real stream's /data would ask for its /start again meanwhile
  s.filter = [](datagram &d) { return d.from != 0; };
  for (int k = 1; k <= 3; k++)
  {
    start_fields st;
    st.stream_id = 9000;
    const bytes b = start_message(st);
    CHECK_EQ(s.inject(0, k, b.data(), b.size()), kAooOk);
  }
  s.peak = 0;
  for (int32_t seq = 0; seq < 40; seq++)
  {
    const bytes d = binary_data(simple_block(9000, seq, loud));
    for (int k = 1; k <= 3; k++) CHECK_EQ(s.inject(0, k, d.data(), d.size()), kAooOk);
    s.run(0.005);
  }
  s.run(0.2);
  printf("  a loud packet, 40 times: peak %.3f\n", s.peak);
  CHECK(s.outputs_finite);
  // clamped, and played (the resampling sinks interpolate a little beyond)
  CHECK(s.peak > 1.9 && s.peak < 2.6);

  // the real stream comes back: its /data asks the source for /start again
  s.filter = [&ls](datagram &d) { ls.watch.observe(d); return true; };
  CHECK(run_until_all_active(s, 3.0));
  s.run(0.5);
  const std::vector<double> rms = output_rms(s, 1.0);
  CHECK(s.outputs_finite);
  for (double r : rms) CHECK(sounds_right(r));
}

// The source side: a binary resend request whose count times 8 overflows a
// 32-bit size_t, requests for blocks the source never had, and exceptions
// other than oscpack's.
static void test_source_requests()
{
  live_session ls;
  session &s = ls.s;
  const long errors = g_error_logs.load();
  // binary data request: stream ID, count, then (seq, frame offset, bitset)
  bytes b(64);
  int n = aoo::binmsg_write_header(b.data(), b.size(), kAooMsgTypeSource, kAooBinMsgCmdData, 1, 1);
  AooByte *it = b.data() + n;
  aoo::write_bytes<int32_t>(ls.watch.stream_id, it);
  aoo::write_bytes<int32_t>(0x20000000, it);
  aoo::write_bytes<int32_t>(ls.watch.seq, it);
  aoo::write_bytes<int16_t>(0, it);
  aoo::write_bytes<uint16_t>(1, it);
  b.resize(it - b.data());
  CHECK_EQ(s.inject(1, 0, b.data(), b.size()), kAooOk);
  // OSC: 300 requests for blocks long gone or never sent
  {
    char buf[4096];
    osc::OutboundPacketStream m(buf, sizeof(buf));
    m << osc::BeginMessage(address(false, kAooMsgData).c_str()) << (int32_t)1 << ls.watch.stream_id;
    for (int i = 0; i < 300; i++) m << (int32_t)(i * 7919 % 100000) << (int32_t)(i % 3 - 1);
    m << osc::EndMessage;
    const bytes o = osc_bytes(m);
    CHECK_EQ(s.inject(1, 0, o.data(), o.size()), kAooOk);
  }
  s.run(0.5);
  CHECK_EQ(g_error_logs.load(), errors); // no error log per bad request
  const std::vector<double> rms = output_rms(s, 1.0);
  CHECK(s.outputs_finite);
  for (double r : rms) CHECK(sounds_right(r));
}

// What a peer can make a source or a sink send: all of it counts against
// the sender's rate limit at our server, so a peer that asks for a lot
// could get the sender's own stream dropped for the whole room. One resend
// request could ask for ~180 blocks, and every ping got a pong, every
// /start request a /start, every /data of an unknown stream a /start
// request. Now: resends within a budget (AOO_RESEND_*), at most one reply
// of each kind per peer per AOO_REPLY_INTERVAL.
static bool has_address(const datagram &d, const std::string &a)
{
  return d.bytes.size() > a.size() && !memcmp(d.bytes.data(), a.c_str(), a.size())
    && d.bytes[a.size()] == 0;
}

static void test_reply_budget()
{
  source_options so;
  so.broadcast = true; // as the glue runs it: the source's own data is broadcast, resends unicast
  session s(so, three_sinks());
  stream_watch watch;
  long resent[4] = {}, pongs = 0, start_replies = 0, start_requests = 0;
  const std::string pong = address(true, kAooMsgPong), start = address(true, kAooMsgStart);
  const std::string start_request = address(false, kAooMsgStart);
  s.filter = [&](datagram &d) {
    watch.observe(d);
    if (d.from == 0 && !d.broadcast && d.bytes.size() >= 4 && d.bytes[0] == (0x80 | kAooMsgTypeSink)
        && (d.bytes[1] & 0x7f) == kAooBinMsgCmdData && d.to < 4)
      resent[d.to]++;
    if (d.from == 0 && d.to == 1 && has_address(d, pong)) pongs++;
    if (d.from == 0 && d.to == 1 && has_address(d, start)) start_replies++;
    if (d.from == 1 && d.to == 0 && has_address(d, start_request)) start_requests++;
    return true;
  };
  s.start();
  CHECK(run_until_all_active(s, 2.0));
  s.run(1.2); // a full resend history
  const double rate = 48000.0 / 240; // the stream's packets per second

  // one maximal binary request: 180 recent blocks
  auto request = [&](int count) {
    bytes b(AOO_MAX_PACKET_SIZE);
    const int n = aoo::binmsg_write_header(b.data(), b.size(), kAooMsgTypeSource, kAooBinMsgCmdData, 1, 1);
    AooByte *it = b.data() + n;
    aoo::write_bytes<int32_t>(watch.stream_id, it);
    aoo::write_bytes<int32_t>(count, it);
    for (int i = 0; i < count; i++)
    {
      aoo::write_bytes<int32_t>(watch.seq - 2 - i, it);
      aoo::write_bytes<int16_t>(-1, it);
      aoo::write_bytes<uint16_t>(0, it);
    }
    b.resize(it - b.data());
    return b;
  };
  const bytes big = request(180);
  CHECK(big.size() <= 1452);
  CHECK_EQ(s.inject(1, 0, big.data(), big.size()), kAooOk);
  s.run(0.005);
  printf("  one request for 180 blocks: %ld resent\n", resent[1]);
  CHECK(resent[1] > 0);
  CHECK(resent[1] <= AOO_RESEND_BURST_SINK);

  // one a block for a second, from two sinks: each within its share, both
  // within the source's
  for (auto &r : resent) r = 0;
  for (int i = 0; i < 200; i++)
  {
    const bytes b = request(180);
    CHECK_EQ(s.inject(1, 0, b.data(), b.size()), kAooOk);
    CHECK_EQ(s.inject(2, 0, b.data(), b.size()), kAooOk);
    s.run(0.005);
  }
  printf("  a second of them from sinks 1 and 2: %ld and %ld resent (%ld to sink 3)\n", resent[1],
         resent[2], resent[3]);
  const double sink_most = AOO_RESEND_BURST_SINK + rate * AOO_RESEND_SHARE_SINK * 1.05;
  CHECK(resent[1] <= sink_most && resent[2] <= sink_most);
  CHECK(resent[1] + resent[2] <= AOO_RESEND_BURST + rate * AOO_RESEND_SHARE * 1.05);
  CHECK(resent[1] > rate * AOO_RESEND_SHARE_SINK * 0.8); // the share is there
  CHECK_EQ(resent[3], 0L);

  // a ping and a /start request every 5 ms for a second
  pongs = start_replies = 0;
  for (int i = 0; i < 200; i++)
  {
    char buf[256];
    osc::OutboundPacketStream m(buf, sizeof(buf));
    m << osc::BeginMessage(address(false, kAooMsgPing).c_str()) << (int32_t)1
      << osc::TimeTag(ntp_from_seconds(s.now)) << osc::EndMessage;
    const bytes p = osc_bytes(m);
    CHECK_EQ(s.inject(1, 0, p.data(), p.size()), kAooOk);
    osc::OutboundPacketStream q(buf, sizeof(buf));
    q << osc::BeginMessage(start_request.c_str()) << (int32_t)1 << aoo_getVersionString() << osc::EndMessage;
    const bytes r = osc_bytes(q);
    CHECK_EQ(s.inject(1, 0, r.data(), r.size()), kAooOk);
    s.run(0.005);
  }
  printf("  200 pings and /start requests in a second: %ld pongs, %ld /start\n", pongs, start_replies);
  const long most = (long)(1.0 / AOO_REPLY_INTERVAL) + 2; // and the source's own pings' timing
  CHECK(pongs <= most);
  CHECK(pongs >= 1);
  CHECK(start_replies <= most);
  CHECK(start_replies >= 1);

  // a sink: data of a stream it doesn't know, 200 a second
  start_requests = 0;
  for (int i = 0; i < 200; i++)
  {
    const bytes d = binary_data(simple_block(watch.stream_id + 12345, i, bytes(30, 0x66)));
    CHECK_EQ(s.inject(0, 1, d.data(), d.size()), kAooOk);
    s.run(0.005);
  }
  printf("  200 /data of an unknown stream in a second: %ld /start requests\n", start_requests);
  CHECK(start_requests <= most);
  CHECK(start_requests >= 1);

  // everybody still plays
  s.run(0.5);
  const std::vector<double> rms = output_rms(s, 1.0);
  CHECK(s.outputs_finite);
  for (double r : rms) CHECK(sounds_right(r));
}

// ---------------------------------------------------------------------------
// The fuzzer
// ---------------------------------------------------------------------------

struct fuzzer
{
  std::mt19937 rng;
  std::vector<datagram> corpus;
  stream_watch watch;
  long sent[4] = {};

  explicit fuzzer(unsigned seed) : rng(seed) {}

  uint32_t below(uint32_t n) { return n ? rng() % n : 0; }
  bool chance(int percent) { return (int)below(100) < percent; }

  int32_t interesting_int()
  {
    static const int32_t v[] = { 0, 1, -1, 2, 3, 4, 6, 7, 8, 15, 16, 17, 127, 128, 255, 256, 257,
      1024, 1500, 4095, 4096, 4097, 32767, 32768, 65535, 65536, 65537, AOO_MAX_BLOCK_DATA_SIZE,
      0x10000000, 0x20000000, INT_MAX, INT_MIN, INT_MIN + 1, -2, -255, -65536 };
    return v[below(sizeof(v) / sizeof(v[0]))];
  }

  int32_t near_or_odd(int32_t base, int spread)
  {
    if (chance(70)) return base + (int32_t)below(2 * spread + 1) - spread;
    return interesting_int();
  }

  bytes random_bytes(size_t n)
  {
    bytes b(n);
    for (auto &x : b) x = (AooByte)rng();
    return b;
  }

  void mutate(bytes &b)
  {
    static const AooByte bytes8[] = { 0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x10, 0x20 };
    const int n = 1 + (int)below(4);
    for (int k = 0; k < n; k++)
    {
      switch (below(7))
      {
        case 0:
          if (!b.empty()) b[below((uint32_t)b.size())] ^= (AooByte)(1u << below(8));
        break;
        case 1:
          if (!b.empty()) b[below((uint32_t)b.size())] = bytes8[below(8)];
        break;
        case 2:
          if (b.size() >= 4)
          {
            const size_t at = below((uint32_t)(b.size() / 4)) * 4;
            aoo::to_bytes<int32_t>(interesting_int(), b.data() + at);
          }
        break;
        case 3:
          b.resize(below((uint32_t)b.size() + 1));
        break;
        case 4:
          if (b.size() < 5000)
          {
            const bytes r = random_bytes(1 + below(chance(10) ? 4300 : 64));
            b.insert(b.end(), r.begin(), r.end());
          }
        break;
        case 5:
          if (!corpus.empty())
          {
            const bytes &o = corpus[below((uint32_t)corpus.size())].bytes;
            const size_t cut = below((uint32_t)b.size() + 1), from = below((uint32_t)o.size() + 1);
            b.resize(cut);
            b.insert(b.end(), o.begin() + from, o.end());
          }
        break;
        default:
          if (!b.empty())
          {
            const size_t at = below((uint32_t)b.size()), len = 1 + below(16);
            for (size_t i = at; i < b.size() && i < at + len; i++) b[i] = (AooByte)rng();
          }
        break;
      }
    }
  }

  // a sink-bound binary /data with every field around its limits
  bytes binary_sink_data()
  {
    bin_data d;
    d.stream_id = chance(80) ? watch.stream_id : interesting_int();
    d.seq = near_or_odd(watch.seq, 40);
    d.channel = (uint8_t)(chance(80) ? 0 : rng());
    d.flags = (uint8_t)(chance(50) ? 0 : below(32));
    size_t plen = chance(50) ? watch.last_payload.size() : below(chance(10) ? 4000 : 400);
    if (chance(50) && !watch.last_payload.empty()) d.payload = watch.last_payload;
    else d.payload = random_bytes(plen);
    if ((d.flags & kAooBinMsgDataStreamMessage) && d.payload.size() >= 12 && chance(60))
    {
      // a plausible message header region at the start
      aoo::to_bytes<int32_t>(chance(50) ? 1 : interesting_int(), d.payload.data());
      aoo::to_bytes<uint16_t>((uint16_t)rng(), d.payload.data() + 4);
      aoo::to_bytes<uint16_t>((uint16_t)below(4), d.payload.data() + 6);
      aoo::to_bytes<uint16_t>(chance(30) ? 0xFFFF : (uint16_t)below(8), d.payload.data() + 8);
      aoo::to_bytes<uint16_t>((uint16_t)(chance(50) ? below(64) : rng()), d.payload.data() + 10);
    }
    d.size = (uint16_t)(chance(80) ? d.payload.size() : (uint32_t)interesting_int());
    d.total = (uint32_t)(chance(50) ? d.payload.size() * (1 + below(3)) : (uint32_t)interesting_int());
    d.nframes = (uint16_t)(chance(50) ? 1 + below(4) : (uint32_t)interesting_int());
    d.frame = (uint16_t)(chance(50) ? below(4) : (uint32_t)interesting_int());
    d.msgsize = (uint32_t)(chance(50) ? 4 * below(8) : (uint32_t)interesting_int());
    static const double rates[] = { 48000, 48000.5, 44100, 0, -1, 1e300, 1e-300,
      std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity() };
    d.sr = rates[below(sizeof(rates) / sizeof(rates[0]))];
    d.tt = ((uint64_t)rng() << 32) | rng();
    return binary_data(d, chance(95) ? 1 : (AooId)interesting_int(), chance(95) ? 1 : (AooId)interesting_int());
  }

  void add_int_or_other(osc::OutboundPacketStream &m, int32_t v)
  {
    switch (below(20))
    {
      case 0: m << osc::Nil; break;
      case 1: m << (float)v; break;
      case 2: m << "x"; break;
      default: m << v; break;
    }
  }

  // a sink-bound OSC message of a random kind
  bytes osc_to_sink()
  {
    char buf[8192];
    osc::OutboundPacketStream m(buf, sizeof(buf));
    const AooId src = chance(90) ? 1 : (AooId)interesting_int();
    switch (below(6))
    {
      case 0:
      {
        start_fields f;
        f.source_id = src;
        f.stream_id = chance(50) ? watch.stream_id + (int32_t)below(3) : interesting_int();
        f.seq_start = near_or_odd(watch.seq, 10);
        f.format_id = (int32_t)below(4);
        static const int32_t ch[] = { 1, 2, 2, 8, 9, 0, -1, 255, 256 };
        static const int32_t sr[] = { 48000, 48000, 24000, 8000, 44100, 0, -48000, 96000 };
        static const int32_t bs[] = { 240, 240, 120, 480, 960, 2880, 2881, 0, -240, 100000 };
        f.channels = ch[below(9)];
        f.samplerate = sr[below(8)];
        f.blocksize = bs[below(10)];
        if (chance(10)) f.codec = chance(50) ? "pcm" : "";
        if (chance(10)) f.version = chance(50) ? "99999999999999999999.1" : "x";
        if (chance(30)) f.application = interesting_int();
        f.latency = near_or_odd(0, 500);
        f.codec_delay = near_or_odd(120, 50);
        f.offset = near_or_odd(0, 100);
        return start_message(f);
      }
      case 1:
        m << osc::BeginMessage(address(true, kAooMsgStop).c_str()) << src
          << (chance(50) ? watch.stream_id : interesting_int()) << near_or_odd(watch.seq, 10)
          << near_or_odd(0, 100) << osc::EndMessage;
      break;
      case 2:
        m << osc::BeginMessage(address(true, kAooMsgDecline).c_str()) << src << interesting_int()
          << osc::EndMessage;
      break;
      case 3:
      {
        m << osc::BeginMessage(address(true, kAooMsgData).c_str()) << src;
        add_int_or_other(m, chance(80) ? watch.stream_id : interesting_int());
        add_int_or_other(m, near_or_odd(watch.seq, 40));
        if (chance(50)) m << osc::Nil; else m << osc::TimeTag(((uint64_t)rng() << 32) | rng());
        if (chance(50)) m << osc::Nil; else m << (chance(50) ? 48000.0 : (double)interesting_int());
        add_int_or_other(m, chance(70) ? 0 : interesting_int());              // channel
        const bytes p = chance(50) ? watch.last_payload : random_bytes(below(600));
        add_int_or_other(m, chance(70) ? (int32_t)p.size() : interesting_int()); // total
        if (chance(70)) m << osc::Nil; else add_int_or_other(m, interesting_int()); // msgsize
        if (chance(60)) m << osc::Nil << osc::Nil;
        else { add_int_or_other(m, interesting_int()); add_int_or_other(m, interesting_int()); }
        if (chance(10)) m << osc::Nil;
        else m << osc::Blob(p.data(), (osc::osc_bundle_element_size_t)p.size());
        m << osc::EndMessage;
      }
      break;
      case 4:
        m << osc::BeginMessage(address(true, kAooMsgPing).c_str()) << src
          << osc::TimeTag(((uint64_t)rng() << 32) | rng()) << osc::EndMessage;
      break;
      default:
        m << osc::BeginMessage(address(true, kAooMsgPong).c_str()) << src
          << osc::TimeTag(rng()) << osc::TimeTag(rng()) << osc::TimeTag(rng()) << osc::EndMessage;
      break;
    }
    return osc_bytes(m);
  }

  // a source-bound message of a random kind
  bytes to_source()
  {
    char buf[8192];
    osc::OutboundPacketStream m(buf, sizeof(buf));
    const AooId sink = chance(90) ? 1 : (AooId)interesting_int();
    switch (below(8))
    {
      case 0:
        m << osc::BeginMessage(address(false, kAooMsgStart).c_str()) << sink
          << (chance(80) ? aoo_getVersionString() : "99999999999999999999.9") << osc::EndMessage;
      break;
      case 1:
        m << osc::BeginMessage(address(false, kAooMsgStop).c_str()) << sink
          << (chance(50) ? watch.stream_id : interesting_int()) << osc::EndMessage;
      break;
      case 2:
      {
        m << osc::BeginMessage(address(false, kAooMsgData).c_str()) << sink
          << (chance(80) ? watch.stream_id : interesting_int());
        const int pairs = (int)below(chance(10) ? 400 : 8);
        for (int i = 0; i < pairs; i++)
        {
          add_int_or_other(m, near_or_odd(watch.seq - 10, 20));
          add_int_or_other(m, chance(50) ? -1 : interesting_int());
        }
        m << osc::EndMessage;
      }
      break;
      case 3:
      {
        m << osc::BeginMessage(address(false, kAooMsgInvite).c_str()) << sink << interesting_int();
        if (chance(50)) m << osc::Nil << osc::Nil;
        else
        {
          const bytes md = random_bytes(below(200));
          m << interesting_int() << osc::Blob(md.data(), (osc::osc_bundle_element_size_t)md.size());
        }
        m << osc::EndMessage;
      }
      break;
      case 4:
        m << osc::BeginMessage(address(false, kAooMsgUninvite).c_str()) << sink
          << (chance(50) ? watch.stream_id : interesting_int()) << osc::EndMessage;
      break;
      case 5:
        m << osc::BeginMessage(address(false, kAooMsgPing).c_str()) << sink
          << osc::TimeTag(rng()) << osc::EndMessage;
      break;
      case 6:
        m << osc::BeginMessage(address(false, kAooMsgPong).c_str()) << sink
          << osc::TimeTag(rng()) << osc::TimeTag(rng()) << osc::TimeTag(rng())
          << (float)(rng() % 100) / 100.0f << osc::EndMessage;
      break;
      default:
      {
        // binary resend request
        bytes b(16 + 8 * 40);
        const int n = aoo::binmsg_write_header(b.data(), b.size(), kAooMsgTypeSource,
          kAooBinMsgCmdData, chance(90) ? 1 : interesting_int(), sink);
        AooByte *it = b.data() + n;
        const int pairs = (int)below(40);
        aoo::write_bytes<int32_t>(chance(80) ? watch.stream_id : interesting_int(), it);
        aoo::write_bytes<int32_t>(chance(70) ? pairs : interesting_int(), it);
        for (int i = 0; i < pairs; i++)
        {
          aoo::write_bytes<int32_t>(near_or_odd(watch.seq - 10, 20), it);
          aoo::write_bytes<int16_t>((int16_t)(chance(50) ? -1 : interesting_int()), it);
          aoo::write_bytes<uint16_t>((uint16_t)rng(), it);
        }
        b.resize(it - b.data());
        return b;
      }
    }
    return osc_bytes(m);
  }

  // one hostile datagram: returns it and sets its destination
  bytes next(int &to)
  {
    const uint32_t kind = below(100);
    if (kind < 40 && !corpus.empty())
    {
      const datagram &d = corpus[below((uint32_t)corpus.size())];
      to = d.to;
      bytes b = d.bytes;
      mutate(b);
      sent[0]++;
      return b;
    }
    to = (int)below(4);
    try
    {
      if (kind < 90)
      {
        sent[1]++;
        if (to == 0) return to_source();
        return chance(60) ? binary_sink_data() : osc_to_sink();
      }
    }
    catch (const osc::Exception &)
    {
      // too large for the buffer: send random bytes instead
    }
    sent[2]++;
    bytes b = random_bytes(below(1500));
    if (chance(40))
    {
      // with a valid start, so that it gets past aoo_parsePattern()
      const std::string a = address(to != 0, kAooMsgData);
      if (chance(50)) b.insert(b.begin(), a.begin(), a.end());
      else if (b.size() >= 4) aoo::binmsg_write_header(b.data(), b.size(),
        to == 0 ? kAooMsgTypeSource : kAooMsgTypeSink, kAooBinMsgCmdData, 1, 1);
    }
    return b;
  }
};

static void run_fuzzer(long iterations, unsigned seed)
{
  fuzzer f(seed);
  session s(source_options(), three_sinks());
  bool capture = true;
  bool lossy = false;
  s.filter = [&](datagram &d) {
    f.watch.observe(d);
    if (capture && f.corpus.size() < 3000) f.corpus.push_back(d);
    // some loss, so that the sinks ask for resends
    if (lossy && d.from == 0 && f.chance(5)) return false;
    return true;
  };

  // capture: binary, then OSC, then binary again, with loss
  s.start();
  CHECK(run_until_all_active(s, 2.0));
  lossy = true;
  s.run(1.0);
  s.source->setBinaryFormat(kAooFalse);
  for (auto &k : s.sinks) k->setBinaryFormat(kAooFalse);
  s.run(1.0);
  s.source->setBinaryFormat(kAooTrue);
  for (auto &k : s.sinks) k->setBinaryFormat(kAooTrue);
  s.run(0.5);
  capture = false;
  lossy = false;
  const size_t corpus_size = f.corpus.size();

  // a few strangers, with a handful of IDs, so that the sources and sinks
  // they create stay few
  sockaddr_in strangers[3] = { make_address(100), make_address(101), make_address(102) };

  const long errors = g_error_logs.load();
  for (long i = 0; i < iterations; i++)
  {
    int to = 0;
    const bytes b = f.next(to);
    if (f.chance(85))
    {
      // from the real peer: the source for a sink, a sink for the source
      const int from = to == 0 ? 1 + (int)f.below(3) : 0;
      s.inject(from, to, b.data(), b.size());
    }
    else
    {
      s.inject_from(strangers[f.below(3)], to, b.data(), b.size());
    }
    s.tick();
    if (!s.outputs_finite) break;
  }
  CHECK(s.outputs_finite);
  CHECK_EQ(g_error_logs.load(), errors);
  printf("  fuzz: %ld datagrams (%ld mutated, %ld built, %ld random) from a corpus of %zu\n",
    iterations, f.sent[0], f.sent[1], f.sent[2], corpus_size);

  // a new stream plays normally; restart it, because a forged /stop from
  // the source's address ends the stream for good (that is AOO's protocol)
  s.filter = nullptr;
  s.source->stopStream(0);
  s.run(0.3);
  s.peak = 0;
  s.start();
  CHECK(run_until_all_active(s, 3.0));
  s.run(0.5);
  const std::vector<double> rms = output_rms(s, 1.0);
  CHECK(s.outputs_finite);
  CHECK(s.peak < 2.0);
  for (size_t k = 0; k < rms.size(); k++)
  {
    printf("  after fuzz: sink %zu RMS %.3f\n", k + 1, rms[k]);
    CHECK(sounds_right(rms[k]));
  }
}

int main(int argc, char **argv)
{
  const long iterations = argc > 1 ? atol(argv[1]) : 60000;
  const unsigned seed = argc > 2 ? (unsigned)strtoul(argv[2], nullptr, 10) : 1234;
  const std::string only = argc > 3 ? argv[3] : "";
  printf("live_fuzz_test: %ld iterations, seed %u\n", iterations, seed);
  fflush(stdout);

  AooSettings settings;
  settings.logFunc = count_log;
  aoo_initialize(&settings);

  const struct { const char *name; void (*fn)(); } tests[] = {
    { "parse_pattern", test_parse_pattern },
    { "packet_size", test_packet_size },
    { "data_fields", test_data_fields },
    { "frames", test_frames },
    { "sequence_jump", test_sequence_jump },
    { "sequence_gap", test_sequence_gap },
    { "stream_messages", test_stream_messages },
    { "start_formats", test_start_formats },
    { "start_delays", test_start_delays },
    { "loud_packet", test_loud_packet },
    { "source_requests", test_source_requests },
    { "reply_budget", test_reply_budget },
  };
  for (auto &t : tests)
    if (only.empty() || only == t.name) t.fn();

  if (only.empty() || only == "fuzz") run_fuzzer(iterations, seed);

  return test_summary("live_fuzz_test");
}
