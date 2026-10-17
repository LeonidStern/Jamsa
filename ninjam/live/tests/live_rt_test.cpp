/*
    Jamsa Live - live_rt_test.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  What AOO's source and sinks do on the audio thread: no heap allocation or
  free in process(), once a stream runs, whether the network is clean or
  loses, delays, reorders and duplicates packets, and when the stream
  restarts or the source has xruns. The heap can take a lock or a page
  fault, which an audio thread must never wait for. Also the rest of our
  real-time changes: stream IDs without the heap, events by polling only,
  a log function that can be replaced at any time.

  This program replaces the global operator new and delete and counts the
  calls made from inside process() (the harness's audio_hook). AOO's own
  real-time allocations come from its memory pool and don't count, unless
  the pool runs out and falls back to the heap.

    live_rt_test            (LIVE_RT_TRACE=1 prints where each counted
                             allocation came from, on glibc)

*/

#include "detail.hpp" // aoo::get_random_id()

#include "live_harness.h"
#include "testutil.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <random>
#include <thread>

#ifdef _WIN32
#include <malloc.h> // _aligned_malloc
#endif

#if defined(__GLIBC__)
#include <execinfo.h>
#include <unistd.h>
#define LIVE_RT_HAVE_BACKTRACE 1
#endif

using namespace live_harness;

// ---------------------------------------------------------------------------
// Counting heap
// ---------------------------------------------------------------------------

static std::atomic<bool> g_in_audio{false};
static std::atomic<long> g_audio_heap_calls{0};
static std::atomic<bool> g_in_network{false};
static std::atomic<long> g_network_heap_calls{0};
static bool g_trace = false;
static int g_traced = 0;
static thread_local bool g_in_trace = false;

static void note_heap_call()
{
  if (g_in_network.load(std::memory_order_relaxed)) g_network_heap_calls++;
  if (!g_in_audio.load(std::memory_order_relaxed)) return;
  g_audio_heap_calls++;
#if LIVE_RT_HAVE_BACKTRACE
  if (g_trace && !g_in_trace && g_traced < 8)
  {
    g_in_trace = true;
    g_traced++;
    void *frames[24];
    const int n = backtrace(frames, 24);
    static const char line[] = "--- heap call on the audio thread:\n";
    if (write(2, line, sizeof(line) - 1) < 0) {}
    backtrace_symbols_fd(frames, n, 2);
    g_in_trace = false;
  }
#endif
}

static void *heap_alloc(std::size_t n)
{
  note_heap_call();
  void *p = std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}

static void heap_free(void *p)
{
  if (!p) return;
  note_heap_call();
  std::free(p);
}

static void *heap_alloc_aligned(std::size_t n, std::size_t align)
{
  note_heap_call();
  if (align < sizeof(void *)) align = sizeof(void *);
#ifdef _WIN32
  void *p = _aligned_malloc(n ? n : 1, align);
#else
  void *p = nullptr;
  if (posix_memalign(&p, align, n ? n : 1) != 0) p = nullptr;
#endif
  if (!p) throw std::bad_alloc();
  return p;
}

static void heap_free_aligned(void *p)
{
  if (!p) return;
  note_heap_call();
#ifdef _WIN32
  _aligned_free(p);
#else
  std::free(p);
#endif
}

void *operator new(std::size_t n) { return heap_alloc(n); }
void *operator new[](std::size_t n) { return heap_alloc(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
  try { return heap_alloc(n); } catch (...) { return nullptr; }
}
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept
{
  try { return heap_alloc(n); } catch (...) { return nullptr; }
}
void operator delete(void *p) noexcept { heap_free(p); }
void operator delete[](void *p) noexcept { heap_free(p); }
void operator delete(void *p, std::size_t) noexcept { heap_free(p); }
void operator delete[](void *p, std::size_t) noexcept { heap_free(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { heap_free(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { heap_free(p); }

void *operator new(std::size_t n, std::align_val_t a) { return heap_alloc_aligned(n, (std::size_t)a); }
void *operator new[](std::size_t n, std::align_val_t a) { return heap_alloc_aligned(n, (std::size_t)a); }
void operator delete(void *p, std::align_val_t) noexcept { heap_free_aligned(p); }
void operator delete[](void *p, std::align_val_t) noexcept { heap_free_aligned(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { heap_free_aligned(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { heap_free_aligned(p); }

static void audio_hook(bool entering)
{
  g_in_audio.store(entering, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

static std::vector<sink_options> three_sinks()
{
  sink_options a, b, c;
  a.dynamic_resampling = true;
  b.blocksize = 240; // the stream's block size
  c.samplerate = 44100;
  c.dynamic_resampling = true;
  return { a, b, c };
}

// runs 'warmup' seconds, then counts the audio thread's heap calls for
// 'seconds'; between() runs once per simulated 100 ms while counting
template<typename F>
static long count_heap_calls(session &s, double warmup, double seconds, F between)
{
  s.audio_hook = audio_hook;
  s.run(warmup);
  const long before = g_audio_heap_calls.load();
  const double end = s.now + seconds;
  while (s.now < end)
  {
    s.run(0.1);
    between();
  }
  return g_audio_heap_calls.load() - before;
}

static bool sounds_right(session &s)
{
  // a sine at 0.5 has an RMS of 0.35; the last block of each sink
  for (size_t k = 1; k < s.nodes.size(); k++)
  {
    const double r = s.block_rms((int)k);
    if (!(r > 0.2 && r < 0.5)) return false;
  }
  return s.outputs_finite;
}

// a clean network
static void test_steady()
{
  session s(source_options(), three_sinks());
  s.start();
  const long n = count_heap_calls(s, 2.0, 10.0, []() {});
  printf("  steady: %ld heap calls in process()\n", n);
  CHECK_EQ(n, 0L);
  CHECK(sounds_right(s));
  for (size_t k = 1; k < s.nodes.size(); k++) CHECK(s.nodes[k].stream_active >= 1);
}

// loss (single and bursts), jitter that reorders, duplicates, both ways
static void test_loss()
{
  session s(source_options(), three_sinks());
  std::mt19937 rng(42);
  std::uniform_real_distribution<double> u(0, 1);
  int burst = 0;
  s.filter = [&](datagram &d) {
    if (burst > 0) { burst--; return false; }
    if (u(rng) < 0.003) { burst = 8; return false; }
    if (u(rng) < 0.05) return false;
    d.due += u(rng) * 0.006; // up to 6 ms of jitter: reorders blocks
    if (u(rng) < 0.01) s.in_flight.push_back(d); // a duplicate
    return true;
  };
  s.start();
  const long n = count_heap_calls(s, 3.0, 20.0, []() {});
  int dropped = 0, resent = 0;
  for (size_t k = 1; k < s.nodes.size(); k++)
  {
    dropped += s.nodes[k].dropped;
    resent += s.nodes[k].resent;
  }
  printf("  loss: %ld heap calls in process() (%d blocks resent, %d dropped)\n", n, resent, dropped);
  CHECK_EQ(n, 0L);
  CHECK(resent > 0);
  CHECK(s.outputs_finite);
}

// the source restarts its stream and has xruns
static void test_restart_and_xruns()
{
  session s(source_options(), three_sinks());
  s.start();
  s.run(1.0);
  // one restart while warming up, so that each sink has set up its decoder
  s.source->stopStream(0);
  s.run(0.2);
  s.start();
  int tick = 0;
  const long n = count_heap_calls(s, 2.0, 12.0, [&]() {
    tick++;
    if (tick % 30 == 0)
    {
      s.source->stopStream(0);
    }
    else if (tick % 30 == 3)
    {
      s.start();
    }
    else if (tick % 10 == 5)
    {
      s.source_skips = 4;
    }
  });
  printf("  restart and xruns: %ld heap calls in process()\n", n);
  CHECK_EQ(n, 0L);
  CHECK(s.outputs_finite);
  for (size_t k = 1; k < s.nodes.size(); k++) CHECK(s.nodes[k].stream_active >= 4);
}

// stream messages: the source copies them on the audio thread, the sinks
// schedule them on theirs (both from the RT pool, which uses the heap for
// blocks over 64 KB, hence the sinks' 16 KB cap)
static void test_stream_messages()
{
  session s(source_options(), three_sinks());
  s.start();
  std::vector<AooByte> payload(15000, 0x5a);
  int tick = 0;
  const long n = count_heap_calls(s, 1.0, 5.0, [&]() {
    AooStreamMessage m;
    m.sampleOffset = 0;
    m.channel = 0;
    m.type = kAooDataBinary;
    m.size = (tick++ % 2) ? 3000 : (AooInt32)payload.size();
    m.data = payload.data();
    audio_hook(true); // addStreamMessage() is called on the audio thread
    s.source->addStreamMessage(m);
    audio_hook(false);
  });
  long received = 0;
  for (size_t k = 1; k < s.nodes.size(); k++) received += s.nodes[k].stream_messages;
  printf("  stream messages: %ld heap calls in process(), %ld messages received\n", n, received);
  CHECK_EQ(n, 0L);
  CHECK(received >= 3 * 40);
  CHECK(s.outputs_finite);
}

// A sink whose process() isn't called (the host stopped processing) while
// its packets keep coming: it kept every one of them for the next
// process(), in frames from the heap, without a bound, and that process()
// then went through all of them. Now it keeps AOO_MAX_QUEUED_PACKETS and
// drops what comes after: no more heap once they are there, a short first
// process(), and the stream back soon after.
static void paused_network_hook(int to, bool entering)
{
  if (to == 1) g_in_network.store(entering, std::memory_order_relaxed);
}

static void test_paused_sink()
{
  session s(source_options(), three_sinks());
  s.start();
  s.run(2.0);
  CHECK(sounds_right(s));
  s.network_hook = paused_network_hook;
  s.nodes[1].paused = true;
  s.run(8.0); // 200 packets a second: the bound is reached after about 5 s
  const long before = g_network_heap_calls.load();
  s.run(20.0);
  const long n = g_network_heap_calls.load() - before;
  s.network_hook = nullptr;
  // the paused sink's next process()
  s.nodes[1].paused = false;
  double first = 0;
  for (;;)
  {
    const auto t0 = std::chrono::steady_clock::now();
    const int k = s.tick();
    if (k != 1) continue;
    first = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    break;
  }
  const bool back = s.run_until([&]() { return sounds_right(s); }, 3.0);
  printf("  paused sink: %ld heap calls receiving 20 s of packets, then a first process() of %.2f ms, %s\n",
         n, first, back ? "playing again" : "NOT playing again");
  CHECK(n < 50);
  CHECK(first < 5);
  CHECK(back);
  s.run(1.0);
  CHECK(sounds_right(s));
}

// get_random_id() makes stream and format IDs, on the audio thread too:
// no heap, not even on a thread's first call, never negative, no repeats
static void test_random_ids()
{
  std::vector<AooId> ids(100000);
  const long before = g_audio_heap_calls.load();
  std::thread t([&]() {
    g_in_audio = true;
    for (auto &id : ids) id = aoo::get_random_id();
    g_in_audio = false;
  });
  t.join();
  // (the thread's own start and end may use the heap, but not while counting)
  CHECK_EQ(g_audio_heap_calls.load() - before, 0L);
  bool negative = false;
  for (AooId id : ids) negative = negative || id < 0;
  CHECK(!negative);
  std::sort(ids.begin(), ids.end());
  CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

static long g_logged = 0;
static void AOO_CALL test_log(AooLogLevel, const AooChar *) { g_logged++; }
static void AOO_CALL test_handler(void *, const AooEvent *, AooThreadLevel) {}

// events only by polling; the log function can be replaced at any time
static void test_event_mode_and_log()
{
  AooSource::Ptr source = AooSource::create(1);
  AooSink::Ptr sink = AooSink::create(1);
  const AooEventHandler handler = test_handler;
  CHECK_EQ(source->setEventHandler(handler, nullptr, kAooEventModeCallback), kAooErrorNotImplemented);
  CHECK_EQ(sink->setEventHandler(handler, nullptr, kAooEventModeCallback), kAooErrorNotImplemented);
  CHECK_EQ(source->setEventHandler(handler, nullptr, kAooEventModePoll), kAooOk);
  CHECK_EQ(sink->setEventHandler(handler, nullptr, kAooEventModePoll), kAooOk);

  aoo_setLogFunction(test_log);
  aoo_logMessage(kAooLogLevelError, "test");
  CHECK_EQ(g_logged, 1L);
  aoo_setLogFunction(nullptr); // silent
  aoo_logMessage(kAooLogLevelError, "test");
  CHECK_EQ(g_logged, 1L);
}

int main()
{
  g_trace = getenv("LIVE_RT_TRACE") != nullptr;
  aoo_initialize(nullptr);
  test_steady();
  test_loss();
  test_restart_and_xruns();
  test_stream_messages();
  test_paused_sink();
  test_random_ids();
  test_event_mode_and_log();
  return test_summary("live_rt_test");
}
