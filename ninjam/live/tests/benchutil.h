/*
    Jamsa Live - benchutil.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  What the benchmarks share (live_bench.cpp here, ../../vst3/tests/njclient_bench.cpp):
  options, CSV, percentiles, clocks, a real-time audio thread's priority, a
  thread's CPU time and a process's memory from /proc (Linux), and the
  music-like signal the players play. POSIX only.

*/

#ifndef JAMSA_LIVE_BENCHUTIL_H
#define JAMSA_LIVE_BENCHUTIL_H

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// Small things
// ---------------------------------------------------------------------------

inline void say(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr);
}

// --name value options after the mode
struct Args
{
  std::map<std::string, std::string> kv;
  bool ok = true;

  Args(int argc, char **argv)
  {
    for (int i = 2; i < argc; i++)
    {
      if (strncmp(argv[i], "--", 2) || i + 1 >= argc)
      {
        say("expected --name value, got %s\n", argv[i]);
        ok = false;
        return;
      }
      kv[argv[i] + 2] = argv[i + 1];
      i++;
    }
  }
  std::string str(const char *k, const char *def) const
  {
    auto it = kv.find(k);
    return it == kv.end() ? def : it->second;
  }
  double num(const char *k, double def) const
  {
    auto it = kv.find(k);
    return it == kv.end() ? def : atof(it->second.c_str());
  }
  std::vector<std::string> list(const char *k, const char *def) const
  {
    std::vector<std::string> out;
    const std::string s = str(k, def);
    size_t at = 0;
    while (at <= s.size())
    {
      size_t c = s.find(',', at);
      if (c == std::string::npos) c = s.size();
      if (c > at) out.push_back(s.substr(at, c - at));
      at = c + 1;
    }
    return out;
  }
  std::vector<double> nums(const char *k, const char *def) const
  {
    std::vector<double> out;
    for (const std::string &s : list(k, def)) out.push_back(atof(s.c_str()));
    return out;
  }
};

// a CSV file: the header once, then rows
struct Csv
{
  FILE *f = stdout;
  explicit Csv(const std::string &path)
  {
    if (!path.empty() && path != "-")
    {
      f = fopen(path.c_str(), "w");
      if (!f)
      {
        say("can't write %s\n", path.c_str());
        exit(2);
      }
    }
  }
  ~Csv()
  {
    if (f != stdout) fclose(f);
  }
  void line(const char *fmt, ...)
  {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
  }
};

// the p-th percentile (0..100), nearest rank
inline double pct(std::vector<double> v, double p)
{
  if (v.empty()) return NAN;
  std::sort(v.begin(), v.end());
  const size_t i = std::min(v.size() - 1, (size_t)floor(p / 100.0 * (double)(v.size() - 1) + 0.5));
  return v[i];
}

inline double mean(const std::vector<double> &v)
{
  if (v.empty()) return NAN;
  double s = 0;
  for (double x : v) s += x;
  return s / (double)v.size();
}

inline double mono_seconds()
{
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

inline double thread_cpu_seconds()
{
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

inline int64_t realtime_ns()
{
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

inline void sleep_until(double t)
{
  timespec ts;
  ts.tv_sec = (time_t)floor(t);
  ts.tv_nsec = (long)((t - floor(t)) * 1e9);
  if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}

inline long this_tid()
{
#ifdef __linux__
  return (long)syscall(SYS_gettid);
#else
  return 0;
#endif
}

// as an audio interface's thread: SCHED_FIFO, where the system lets us
inline bool make_realtime(int priority)
{
  sched_param sp;
  memset(&sp, 0, sizeof(sp));
  sp.sched_priority = priority;
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
}

#ifdef __linux__
// a thread's time on the CPU so far (ns), from /proc/self/task/<tid>/schedstat; -1 if unknown
inline int64_t task_cpu_ns(long tid)
{
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/task/%ld/schedstat", tid);
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  long long run = -1;
  if (fscanf(f, "%lld", &run) != 1) run = -1;
  fclose(f);
  return run;
}

// a thread's scheduling policy (0 = normal), from /proc/self/task/<tid>/stat
inline int task_policy(long tid)
{
  char path[64], buf[1024];
  snprintf(path, sizeof(path), "/proc/self/task/%ld/stat", tid);
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  const char *p = strrchr(buf, ')');
  if (!p) return -1;
  // fields after the name start with field 3 (state); policy is field 41
  int field = 2;
  for (const char *q = p + 1; *q; q++)
  {
    if (*q == ' ')
    {
      field++;
      if (field == 41) return atoi(q + 1);
    }
  }
  return -1;
}

// this process's threads
inline std::vector<long> task_ids()
{
  std::vector<long> out;
  DIR *d = opendir("/proc/self/task");
  if (!d) return out;
  while (dirent *e = readdir(d))
    if (e->d_name[0] >= '0' && e->d_name[0] <= '9') out.push_back(atol(e->d_name));
  closedir(d);
  std::sort(out.begin(), out.end());
  return out;
}

// a line of /proc/<pid>/status, in kB ("VmRSS", "RssAnon", "VmHWM"); -1 if not there
inline long status_kb(int pid, const char *key)
{
  char path[64], line[256];
  snprintf(path, sizeof(path), "/proc/%d/status", pid);
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  long v = -1;
  const size_t kl = strlen(key);
  while (fgets(line, sizeof(line), f))
    if (!strncmp(line, key, kl) && line[kl] == ':') v = atol(line + kl + 1);
  fclose(f);
  return v;
}
#else
inline int64_t task_cpu_ns(long) { return -1; }
inline int task_policy(long) { return -1; }
inline std::vector<long> task_ids() { return std::vector<long>(); }
inline long status_kb(int, const char *) { return -1; }
#endif

// ---------------------------------------------------------------------------
// Signals
// ---------------------------------------------------------------------------

// A stand-in for music: the three notes of a chord with four harmonics each,
// each note swelling and fading at its own rate, and a little noise; the
// right channel mixes the notes otherwise. "noise" is white noise (about the
// most an Opus stream of a given bitrate can need). Made once, 4 s of it, and
// played in a loop: the audio threads spend their time in Live, not here.
struct Signal
{
  std::vector<float> left, right;

  void init(bool noise, double rate, int seed)
  {
    const size_t len = (size_t)(4 * rate);
    left.assign(len, 0.0f);
    right.assign(len, 0.0f);
    uint32_t rng = 0x9E3779B9u * (uint32_t)(seed + 1);
    auto white = [&rng]() {
      rng = rng * 1664525u + 1013904223u;
      return (float)((double)(rng >> 8) / 8388608.0 - 1.0);
    };
    const double transpose = pow(2.0, (seed % 7) / 12.0);
    const double f[3] = { 220.0 * transpose, 277.18 * transpose, 329.63 * transpose };
    // (swells of whole cycles in 4 s, so the loop doesn't click)
    const double swell[3] = { 0.25, 0.5, 0.75 };
    for (size_t i = 0; i < len; i++)
    {
      if (noise)
      {
        left[i] = 0.25f * white();
        right[i] = 0.25f * white();
        continue;
      }
      const double t = (double)i / rate;
      double note[3];
      for (int k = 0; k < 3; k++)
      {
        double v = 0;
        for (int h = 1; h <= 4; h++) v += sin(6.283185307179586 * f[k] * h * t) / h;
        note[k] = v * (0.55 + 0.45 * sin(6.283185307179586 * swell[k] * t + k));
      }
      const float nz = 0.01f * white();
      left[i] = (float)(0.16 * (note[0] + 0.8 * note[1] + 0.5 * note[2])) + nz;
      right[i] = (float)(0.16 * (0.5 * note[0] + 0.8 * note[1] + note[2])) + nz;
    }
  }
  void fill(int64_t index, float *l, float *r, int n) const
  {
    const size_t len = left.size();
    size_t at = (size_t)(index % (int64_t)len);
    for (int i = 0; i < n; i++)
    {
      l[i] = left[at];
      r[i] = right[at];
      if (++at == len) at = 0;
    }
  }
};

} // namespace bench

#endif
