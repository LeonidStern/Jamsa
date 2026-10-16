/*
    Jamsa Live - live_aoo_test.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Tests of the AOO parts Live mode leans on.

  The lock-free queue and the RT memory pool tests come from AOO's own
  tests/test_concurrent_queue.cpp and tests/test_rt_memory_pool.cpp, which
  only print what happened and assert() at the end (nothing in Release).
  Here they check what they promise: every value comes out once and in the
  order its producer pushed it; no two blocks of the pool overlap; a block
  freed is reused rather than taken again from the pool's memory.

  Then one test per AOO bug fixed in our copy (README.md, "Bugs fixed"),
  each failing without its fix, most of them through a source and sinks on
  the virtual network of live_harness.h.

*/

#include "common/lockfree.hpp"
#include "common/sync.hpp"
#include "memory.hpp"
#include "resampler.hpp"
#include "rt_memory_pool.hpp"

#include "live_harness.h"
#include "testutil.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <memory>
#include <new>
#include <random>
#include <thread>
#include <vector>

typedef std::chrono::steady_clock test_clock;

static test_clock::time_point deadline(double seconds)
{
  return test_clock::now() + std::chrono::duration_cast<test_clock::duration>(
    std::chrono::duration<double>(seconds));
}

// ---------------------------------------------------------------------------
// The lock-free queue
// ---------------------------------------------------------------------------

// Producers push (producer << 40 | sequence) in bursts while the main thread
// pops; each producer's values must come out complete and in order.
template<bool multi_producer>
static void test_queue_threads(int producers, double seconds)
{
  aoo::lockfree::concurrent_queue<uint64_t, multi_producer> queue;
  std::atomic<long> backlog{0};
  std::atomic<int> running{producers};
  std::vector<uint64_t> pushed(producers, 0);
  std::vector<std::thread> threads;
  const auto end = deadline(seconds);

  for (int p = 0; p < producers; p++)
  {
    threads.emplace_back([&, p]() {
      std::mt19937 gen(1234 + p);
      std::uniform_int_distribution<int> burst(1, 10), pause(0, 100);
      uint64_t seq = 0;
      while (test_clock::now() < end)
      {
        // keep the queue from growing without bound if the consumer lags
        if (backlog.load(std::memory_order_relaxed) < 10000)
        {
          const int n = burst(gen);
          for (int i = 0; i < n; i++) queue.push(((uint64_t)p << 40) | seq++);
          backlog += n;
        }
        for (int k = pause(gen); k > 0; k--) aoo::sync::pause_cpu();
      }
      pushed[p] = seq;
      running--;
    });
  }

  std::vector<uint64_t> next(producers, 0);
  bool in_order = true, known_producer = true;
  uint64_t popped = 0;
  for (;;)
  {
    // read before draining: once no producer runs, one more drain gets all
    const bool done = running.load() == 0;
    uint64_t v;
    while (queue.pop(v))
    {
      const uint64_t p = v >> 40, seq = v & ((1ull << 40) - 1);
      if (p >= (uint64_t)producers) known_producer = false;
      else
      {
        if (seq != next[p]) in_order = false;
        next[p] = seq + 1;
      }
      popped++;
      backlog--;
    }
    if (done) break;
    std::this_thread::yield();
  }
  for (auto &t : threads) t.join();

  uint64_t total = 0;
  for (int p = 0; p < producers; p++)
  {
    total += pushed[p];
    CHECK_EQ(next[p], pushed[p]);
  }
  CHECK(known_producer);
  CHECK(in_order);
  CHECK_EQ(popped, total);
  CHECK(total > 0);
  CHECK(queue.empty());
  printf("  queue (%s producer): %llu values from %d thread(s)\n",
    multi_producer ? "multi" : "single", (unsigned long long)total, producers);
}

static std::atomic<long> g_queue_allocations{0};
static long g_queue_allocation_limit = LONG_MAX;

// std::allocator that counts its allocations and throws past a limit
template<typename T>
struct counting_allocator
{
  typedef T value_type;
  counting_allocator() {}
  template<typename U> counting_allocator(const counting_allocator<U> &) {}
  T *allocate(size_t n)
  {
    if (++g_queue_allocations > g_queue_allocation_limit) throw std::bad_alloc();
    return std::allocator<T>().allocate(n);
  }
  void deallocate(T *p, size_t n) { std::allocator<T>().deallocate(p, n); }
  template<typename U> bool operator==(const counting_allocator<U> &) const { return true; }
  template<typename U> bool operator!=(const counting_allocator<U> &) const { return false; }
};

// reserve(n) makes room for n values in flight without allocating
template<bool multi_producer>
static void test_queue_reserve()
{
  aoo::lockfree::concurrent_queue<int, multi_producer, counting_allocator<int>> queue;
  queue.reserve(16);
  g_queue_allocations = 0;
  for (int round = 0; round < 100; round++)
  {
    for (int i = 0; i < 16; i++) queue.push(round * 16 + i);
    int v = -1, count = 0;
    bool in_order = true;
    while (queue.pop(v))
    {
      if (v != round * 16 + count) in_order = false;
      count++;
    }
    CHECK_EQ(count, 16);
    CHECK(in_order);
  }
  CHECK_EQ(g_queue_allocations.load(), 0L);

  // the 17th value in flight needs a new node
  for (int i = 0; i < 17; i++) queue.push(i);
  CHECK_EQ(g_queue_allocations.load(), 1L);
  int v;
  while (queue.pop(v)) {}

  // Fixed: asking for fewer nodes than are already free made the count
  // wrap around, and reserve() allocated until memory ran out.
  // (The limit keeps the broken version from eating all memory.)
  g_queue_allocations = 0;
  g_queue_allocation_limit = 1000;
  bool threw = false;
  try { queue.reserve(8); }
  catch (const std::bad_alloc &) { threw = true; }
  CHECK(!threw);
  CHECK_EQ(g_queue_allocations.load(), 0L);
  g_queue_allocations = 0;
  threw = false;
  try { queue.reserve(20); } // 17 free, so 3 more
  catch (const std::bad_alloc &) { threw = true; }
  CHECK(!threw);
  CHECK_EQ(g_queue_allocations.load(), 3L);
  g_queue_allocation_limit = LONG_MAX;
}

// ---------------------------------------------------------------------------
// The RT memory pool
// ---------------------------------------------------------------------------

// Threads take blocks of random sizes (most small, some above the pool's
// 64 KB limit, which come from the heap), fill each with a byte of their
// own and check it is still there before they give the block back: two
// threads handed overlapping blocks would see each other's bytes.
static void test_pool_threads(size_t pool_size, int nthreads, double seconds)
{
  aoo::rt_memory_pool<true> pool;
  pool.resize(pool_size);
  std::atomic<long> allocations{0}, bad_pattern{0}, bad_alignment{0}, null_blocks{0};
  const auto end = deadline(seconds);

  std::vector<std::thread> threads;
  for (int t = 0; t < nthreads; t++)
  {
    threads.emplace_back([&, t]() {
      std::mt19937 gen(99 + t);
      std::uniform_real_distribution<double> unit(0.0, 1.0);
      std::uniform_int_distribution<int> nblocks(0, 10), pause(0, 100);
      struct block { unsigned char *p; size_t size; unsigned char fill; };
      std::vector<block> blocks;
      blocks.reserve(10);
      unsigned counter = 0;
      while (test_clock::now() < end)
      {
        blocks.clear();
        for (int n = nblocks(gen); n > 0; n--)
        {
          const double r = unit(gen);
          const size_t size = std::max<size_t>(1, (size_t)(r * r * r * 70000));
          unsigned char *p = (unsigned char *)pool.allocate(size);
          if (!p) { null_blocks++; continue; }
          allocations++;
          if ((uintptr_t)p % 16) bad_alignment++;
          // 32 values per thread, so no two threads ever write the same byte
          const unsigned char fill = (unsigned char)(t * 32 + 1 + counter++ % 31);
          memset(p, fill, size);
          blocks.push_back({ p, size, fill });
        }
        for (int k = pause(gen); k > 0; k--) aoo::sync::pause_cpu();
        for (auto &b : blocks)
        {
          for (size_t i = 0; i < b.size; i++)
          {
            if (b.p[i] != b.fill) { bad_pattern++; break; }
          }
          pool.deallocate(b.p, b.size);
        }
      }
    });
  }
  for (auto &t : threads) t.join();

  CHECK_EQ(bad_pattern.load(), 0L);
  CHECK_EQ(bad_alignment.load(), 0L);
  CHECK_EQ(null_blocks.load(), 0L);
  CHECK(allocations.load() > 0);
  CHECK(pool.memory_usage() <= pool.capacity());
  printf("  pool (%zu bytes, %d threads): %ld blocks, %zu bytes used\n",
    pool_size, nthreads, allocations.load(), pool.memory_usage());
}

// a freed 32 KB block is split to serve three 8 KB requests
static void test_pool_reuse()
{
  aoo::rt_memory_pool<true> pool;
  pool.resize(1 << 20);
  void *big = pool.allocate(32768);
  CHECK(big != nullptr);
  const size_t used = pool.memory_usage();
  pool.deallocate(big, 32768);

  char *parts[3];
  for (int i = 0; i < 3; i++)
  {
    parts[i] = (char *)pool.allocate(8192);
    CHECK(parts[i] != nullptr);
  }
  CHECK_EQ(pool.memory_usage(), used);
  for (int i = 0; i < 3; i++)
  {
    // all inside the old block, none overlapping another
    CHECK(parts[i] >= (char *)big && parts[i] + 8192 <= (char *)big + used);
    for (int j = 0; j < i; j++)
      CHECK(parts[i] + 8192 <= parts[j] || parts[j] + 8192 <= parts[i]);
  }
  for (int i = 0; i < 3; i++) pool.deallocate(parts[i], 8192);
}

// the same sizes over and over, in a different order each round, are served
// from the blocks freed in the round before and the pool stops growing
static void test_pool_steady()
{
  aoo::rt_memory_pool<true> pool;
  pool.resize(8 << 20);
  std::mt19937 gen(7);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  std::vector<size_t> sizes(300);
  for (auto &s : sizes)
  {
    const double r = unit(gen);
    s = 1 + (size_t)(r * r * r * 65534);
  }
  std::vector<size_t> order(sizes.size());
  for (size_t i = 0; i < order.size(); i++) order[i] = i;
  std::vector<void *> blocks(sizes.size());

  size_t first_round = 0;
  bool all_allocated = true;
  for (int round = 0; round < 50; round++)
  {
    std::shuffle(order.begin(), order.end(), gen);
    for (size_t i : order)
    {
      blocks[i] = pool.allocate(sizes[i]);
      if (!blocks[i]) all_allocated = false;
    }
    std::shuffle(order.begin(), order.end(), gen);
    for (size_t i : order)
    {
      if (blocks[i]) pool.deallocate(blocks[i], sizes[i]);
    }
    if (round == 0) first_round = pool.memory_usage();
  }
  CHECK(all_allocated);
  CHECK(first_round > 0);
  CHECK_EQ(pool.memory_usage(), first_round);
}

// a pool that may not grow says no once it is full, and takes its blocks back
static void test_pool_exhausted()
{
  aoo::rt_memory_pool<false> pool;
  pool.resize(65536);
  std::vector<void *> blocks;
  for (int i = 0; i < 20; i++)
  {
    void *p = pool.allocate(4096);
    if (!p) break;
    blocks.push_back(p);
  }
  CHECK_EQ(blocks.size(), (size_t)16);
  CHECK(pool.allocate(64) == nullptr);
  for (void *p : blocks) pool.deallocate(p, 4096);

  blocks.clear();
  for (int i = 0; i < 16; i++)
  {
    void *p = pool.allocate(4096);
    CHECK(p != nullptr);
    if (p) blocks.push_back(p);
  }
  CHECK_EQ(pool.memory_usage(), (size_t)65536);
  for (void *p : blocks) pool.deallocate(p, 4096);

  CHECK(pool.allocate(0) == nullptr);
  // above the pool's limit the heap serves it, even here
  void *big = pool.allocate(100000);
  CHECK(big != nullptr);
  if (big) pool.deallocate(big, 100000);
}

// ---------------------------------------------------------------------------
// AOO bugs fixed in our copy
// ---------------------------------------------------------------------------

using namespace live_harness;

static bool run_until_active(session &s, int node, double seconds)
{
  return s.run_until([&]() { return s.nodes[node].stream_active > 0; }, seconds);
}

// The resampler kept its buffer when only the channel count changed, but the
// buffer's layout depends on it: writes before the block, and a bad free.
static void test_resampler_channels()
{
  aoo::dynamic_resampler r;
  // (7 * 2.5 + 3) * 2 channels == (3 * 2.5 + 3) * 4 channels samples
  r.setup(7, 7, false, 48000, 48000, true, 2, kAooResampleCubic);
  r.setup(3, 3, false, 48000, 48000, true, 4, kAooResampleCubic);
  AooSample in[3 * 4], out[3 * 4];
  for (int i = 0; i < 12; i++) { in[i] = (AooSample)(i + 1); out[i] = 0; }
  CHECK(r.write(in, 3));
  CHECK(r.read(out, 3));
  bool same = true;
  for (int i = 0; i < 12; i++) if (out[i] != in[i]) same = false;
  CHECK(same);
}

// The allocators' converting assignment fell off its end without a return.
static void test_allocator_assignment()
{
  aoo::allocator<int> a;
  aoo::allocator<char> b;
  CHECK(&(a = b) == &a);
  aoo::rt_allocator<int> c;
  aoo::rt_allocator<char> d;
  CHECK(&(c = d) == &c);
}

// AooSource::getSinkChannelOffset() set the offset (to the caller's variable)
// instead of getting it; AooSink::getDynamicResampling() took its result by
// value.
static void test_getters()
{
  session s(source_options(), { sink_options() });
  const AooEndpoint sink = s.nodes[1].endpoint();
  CHECK_EQ(s.source->setSinkChannelOffset(sink, 1), kAooOk);
  AooInt32 offset = -1;
  CHECK_EQ(s.source->getSinkChannelOffset(sink, offset), kAooOk);
  CHECK_EQ(offset, 1);
  offset = -1;
  s.source->getSinkChannelOffset(sink, offset);
  CHECK_EQ(offset, 1);

  AooBool on = kAooFalse;
  s.sinks[0]->setDynamicResampling(kAooTrue);
  CHECK_EQ(s.sinks[0]->getDynamicResampling(on), kAooOk);
  CHECK(on == kAooTrue);
  s.sinks[0]->setDynamicResampling(kAooFalse);
  s.sinks[0]->getDynamicResampling(on);
  CHECK(on == kAooFalse);
}

// The sink ran a codec control for its decoder through the encoder's
// control function, on the decoder's state: the Opus decoder's latency came
// back as the encoder's (120 samples), and setters wrote into the decoder.
static void test_sink_codec_control()
{
  session s(source_options(), { sink_options() });
  s.start();
  CHECK(run_until_active(s, 1, 2.0));
  const AooEndpoint source = s.nodes[0].endpoint();
  AooInt32 latency = -1;
  CHECK_EQ(s.sinks[0]->codecControl(kAooCodecOpus, kAooCodecCtlGetLatency,
    (AooIntPtr)&source, &latency, sizeof(latency)), kAooOk);
  CHECK_EQ(latency, 0);
  AooInt32 encoder_latency = -1;
  CHECK_EQ(s.source->codecControl(kAooCodecOpus, kAooCodecCtlGetLatency, 0,
    &encoder_latency, sizeof(encoder_latency)), kAooOk);
  CHECK_EQ(encoder_latency, 120); // 2.5 ms at 48 kHz
}

// When the source's audio thread misses blocks it sends empty "xrun" blocks
// to fill the time. In the OSC format the sink took them for one-frame
// blocks that never completed: dropped, and asked for again and again.
static void test_xrun_blocks(bool binary)
{
  source_options so;
  so.binary = binary;
  session s(so, { sink_options() });
  s.start();
  CHECK(run_until_active(s, 1, 2.0));
  s.run(0.5);
  const int dropped = s.nodes[1].dropped;
  s.source_skips = 30; // 40 ms
  s.run(30 * 64 / 48000.0);
  s.source->reportXRun(30 * 64);
  s.run(1.0);
  CHECK(s.nodes[1].xrun > 0);
  CHECK_EQ(s.nodes[1].dropped, dropped);
  CHECK(s.outputs_finite);
  printf("  xrun blocks (%s): %d empty, %d dropped\n", binary ? "binary" : "OSC",
    s.nodes[1].xrun, s.nodes[1].dropped - dropped);
}

// startStream() hides the sample offset in the metadata's data pointer.
// The first /start message took it out and put the pointer back, so every
// later /start of the stream (a sink that joins later, or one that asks
// again because the first was lost) sent the pointer as the offset, and that
// sink never became active.
static void test_start_again_with_metadata()
{
  source_options so;
  so.add_sinks = false;
  session s(so, { sink_options(), sink_options() });
  s.source->addSink(s.nodes[1].endpoint(), kAooTrue);
  static const char text[] = "jamsa live";
  AooData md;
  md.type = kAooDataText;
  md.data = (const AooByte *)text;
  md.size = sizeof(text);
  s.start(&md, 0);
  CHECK(run_until_active(s, 1, 2.0));
  s.run(0.2);
  s.source->addSink(s.nodes[2].endpoint(), kAooTrue);
  CHECK(run_until_active(s, 2, 2.0));
  for (int k = 1; k <= 2; k++)
  {
    CHECK(s.nodes[k].start_had_metadata);
    CHECK(s.nodes[k].start_metadata.size() == sizeof(text) &&
      !memcmp(s.nodes[k].start_metadata.data(), text, sizeof(text)));
    CHECK(s.nodes[k].last_active_offset >= 0 && s.nodes[k].last_active_offset < 64);
  }
}

int main()
{
  printf("live_aoo_test\n");
  aoo_initialize(nullptr);

  test_queue_threads<true>(3, 0.3);
  test_queue_threads<false>(1, 0.2);
  test_queue_reserve<true>();
  test_queue_reserve<false>();

  test_pool_threads(16 << 20, 8, 0.3);
  test_pool_threads(256 << 10, 8, 0.2); // runs out and falls back to the heap
  test_pool_reuse();
  test_pool_steady();
  test_pool_exhausted();

  test_resampler_channels();
  test_allocator_assignment();
  test_getters();
  test_sink_codec_control();
  test_xrun_blocks(true);
  test_xrun_blocks(false);
  test_start_again_with_metadata();

  return test_summary("live_aoo_test");
}
