/*
    NINJAM VST3 - njclient_bench.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  A benchmark, not a test (ctest doesn't run it): what interval mode costs,
  measured as ../../live/tests/live_bench.cpp measures Live, to compare the
  two.

  njclient_bench cpu [--players 1,3,5,7] [--blocks 64,128,256,512] [--kbps 510]
                     [--seconds 20] [--reps 3] [--rate 48000] [--out FILE]

  This process holds one NJClient as the plug-in has it (Opus, its stereo
  input sent, each player on an output pair of its own, 18 outputs), with a
  network thread that calls Run() as the engine's does (until it has nothing
  more to do, then 10 ms of sleep) and an audio thread paced by the clock
  (SCHED_FIFO when the system allows) that calls AudioProc() and times each
  call. A child process holds the server (ninjamsrv_embedded) and N other
  players, each an NJClient with threads of its own, all sending music-like
  Opus intervals at --kbps (4 s intervals: 120 BPM, 8 BPI). Once our
  NJClient plays all N, each run measures: AudioProc()'s wall and CPU time
  per block (decoding the N players' intervals happens there; encoding ours
  happens in Run()), the audio and network threads' CPU, and the bytes our
  TCP connection sent and received (TCP_INFO), with an estimate of them on
  the internet's wire (IPv4 and TCP headers with time stamps, 52 bytes per
  1448-byte segment, and an ACK for every second segment the other way).

  Linux only.

*/

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "njclient.h"
#include "ninjamsrv_embedded.h"

#include "../../live/tests/benchutil.h"

using namespace bench;

static const int NCH = 18; // the plug-in's outputs: main and 8 player pairs

// the kernel's struct tcp_info up to the byte counters (a stable layout; <linux/tcp.h>
// can't be included next to <netinet/tcp.h>, which jnetlib brings)
struct TcpInfo
{
  uint8_t state, ca_state, retransmits, probes, backoff, options, wscale, flags;
  uint32_t rto, ato, snd_mss, rcv_mss, unacked, sacked, lost, retrans, fackets;
  uint32_t last_data_sent, last_ack_sent, last_data_recv, last_ack_recv;
  uint32_t pmtu, rcv_ssthresh, rtt, rttvar, snd_ssthresh, snd_cwnd, advmss, reordering;
  uint32_t rcv_rtt, rcv_space, total_retrans;
  uint64_t pacing_rate, max_pacing_rate, bytes_acked, bytes_received;
  uint32_t segs_out, segs_in;
};

// bytes this process's TCP connections sent (and were acknowledged) and received
static void tcp_bytes(double *sent, double *received)
{
  *sent = *received = 0;
  DIR *d = opendir("/proc/self/fd");
  if (!d) return;
  while (dirent *e = readdir(d))
  {
    const int fd = atoi(e->d_name);
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    TcpInfo ti;
    memset(&ti, 0, sizeof(ti));
    socklen_t len = sizeof(ti);
    if (getsockopt(fd, IPPROTO_TCP, 11 /* TCP_INFO */, &ti, &len) == 0 && len >= sizeof(ti))
    {
      *sent += (double)ti.bytes_acked;
      *received += (double)ti.bytes_received;
    }
  }
  closedir(d);
}

// A player: an NJClient, its network thread and its audio thread.
struct Player
{
  NJClient client;
  double rate = 48000;
  int block = 256;
  Signal signal;
  std::thread net, audio;
  std::atomic<bool> quit{false};
  std::atomic<long> netTid{0}, audioTid{0};
  std::atomic<bool> audioRt{false};
  std::mutex mutex;
  std::condition_variable cv;
  // measuring (the main thread turns 'recording' on and off)
  std::atomic<bool> recording{false};
  std::atomic<size_t> count{0};
  std::vector<float> wallUs, cpuUs, lateUs;
  std::atomic<float> peak{0};

  void setup(int kbps, int seed)
  {
    client.config_metronome = 0.0f;
    client.config_metronome_mute = true;
    client.config_remote_autochan = 2; // each player on an output pair of its own, as in the plug-in
    client.config_remote_autochan_nch = NCH;
    client.SetLocalChannelInfo(0, "music", true, 0 | 1024, true, kbps, true, true);
    client.SetLocalChannelMonitoring(0, true, 0.0f, false, 0.0f, true, true, false, false); // not heard here
    signal.init(false, rate, seed);
  }

  void start(double r, int b, double seconds)
  {
    rate = r;
    block = b;
    const size_t cap = (size_t)(seconds * r / b) + 1000;
    wallUs.assign(cap, 0.0f);
    cpuUs.assign(cap, 0.0f);
    lateUs.assign(cap, 0.0f);
    net = std::thread([this] { netLoop(); });
    audio = std::thread([this] { audioLoop(); });
  }

  void stop()
  {
    quit.store(true);
    cv.notify_all();
    if (audio.joinable()) audio.join();
    if (net.joinable()) net.join();
  }

  // as Engine::threadProc
  void netLoop()
  {
    netTid.store(this_tid());
    while (!quit.load())
    {
      for (int i = 0; i < 100 && !quit.load(); i++)
        if (client.Run()) break;
      std::unique_lock<std::mutex> lock(mutex);
      cv.wait_for(lock, std::chrono::milliseconds(10));
    }
  }

  void audioLoop()
  {
    audioTid.store(this_tid());
    audioRt.store(make_realtime(70));
    std::vector<float> inL(block), inR(block);
    std::vector<std::vector<float>> outs(NCH, std::vector<float>(block, 0.0f));
    float *in[2] = { inL.data(), inR.data() };
    float *out[NCH];
    for (int c = 0; c < NCH; c++) out[c] = outs[c].data();
    const double start = mono_seconds() + 0.005;
    for (uint64_t n = 0; !quit.load(std::memory_order_relaxed); n++)
    {
      const double due = start + (double)n * block / rate;
      sleep_until(due);
      const double t0 = mono_seconds();
      signal.fill((int64_t)n * block, inL.data(), inR.data(), block);
      const double c0 = thread_cpu_seconds(), w0 = mono_seconds();
      client.AudioProc(in, 2, out, NCH, block, (int)rate);
      const double w1 = mono_seconds(), c1 = thread_cpu_seconds();
      float pk = 0;
      for (int i = 0; i < block; i++) pk = std::max(pk, fabsf(outs[0][i]));
      peak.store(std::max(peak.load(std::memory_order_relaxed), pk), std::memory_order_relaxed);
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
    if (read(fd, &c, 1) <= 0) return false;
    if (c == '\n') return true;
    *line += c;
  }
}

static void write_line(int fd, const std::string &s)
{
  const std::string l = s + "\n";
  if (write(fd, l.data(), l.size()) != (ssize_t)l.size()) {}
}

static void server_log(void *, const char *text)
{
  if (getenv("NJCLIENT_BENCH_VERBOSE")) fprintf(stderr, "server: %s\n", text);
}

// The child: the server and the other players, until told to quit.
static void run_room(int others, double rate, int block, int kbps, int cmdFd, int replyFd)
{
  JNL::open_socketlib();
  const char *t = getenv("TMPDIR");
  std::string dir = t && *t ? t : "/tmp";
  if (dir.back() != '/') dir += "/";
  const std::string cfg = dir + "njclient-bench-" + std::to_string((int)getpid()) + ".cfg";
  int port = -1;
  for (int p = 2401; p < 2499 && port < 0; p++)
  {
    FILE *fp = fopen(cfg.c_str(), "w");
    if (!fp) break;
    fprintf(fp, "Port %d\nMaxUsers 16\nMaxChannels 32 32\nAnonymousUsers multi\nAnonymousUsersCanChat yes\n"
                "AnonymousMaskIP no\nDefaultBPM 120\nDefaultBPI 8\nAllowCodecs opus\nRequireCodec opus\nOpusBitrate %d\n",
            p, kbps);
    fclose(fp);
    const int r = ninjamsrv_embedded_start(cfg.c_str(), server_log, nullptr);
    if (r == NINJAMSRV_OK) port = p;
    else if (r != NINJAMSRV_LISTEN_ERROR) break;
  }
  if (port < 0)
  {
    write_line(replyFd, "error: the server didn't start");
    _exit(1);
  }
  std::vector<std::unique_ptr<Player>> players;
  const std::string addr = "127.0.0.1:" + std::to_string(port);
  for (int i = 0; i < others; i++)
  {
    std::unique_ptr<Player> p(new Player);
    p->setup(kbps, i + 2);
    p->client.Connect(addr.c_str(), ("anonymous:p" + std::to_string(i + 2)).c_str(), "");
    p->start(rate, block, 0);
    players.push_back(std::move(p));
  }
  write_line(replyFd, "port " + std::to_string(port));
  for (;;)
  {
    std::string cmd;
    if (!read_line(cmdFd, &cmd, 3600) || cmd == "quit") break;
  }
  // the threads first: NJClient isn't to be disconnected while its Run() or AudioProc() runs
  for (auto &p : players) p->stop();
  for (auto &p : players) p->client.Disconnect();
  ninjamsrv_embedded_stop();
  remove(cfg.c_str());
  JNL::close_socketlib();
  _exit(0);
}

struct RunResult
{
  bool ok = false;
  std::string why;
  std::vector<double> wall, cpu, late;
  double seconds = 0, audioCpuPct = -1, netCpuPct = -1, upBytes = 0, downBytes = 0;
  bool audioRt = false;
  int users = 0;
};

static RunResult run_once(int others, double rate, int block, int kbps, double seconds)
{
  RunResult res;
  int down[2], up[2];
  if (pipe(down) || pipe(up))
  {
    res.why = "pipe";
    return res;
  }
  fflush(stdout);
  fflush(stderr);
  const pid_t pid = fork();
  if (pid < 0)
  {
    res.why = "fork";
    return res;
  }
  if (pid == 0)
  {
    close(down[1]);
    close(up[0]);
    run_room(others, rate, block, kbps, down[0], up[1]);
  }
  close(down[0]);
  close(up[1]);
  std::string line;
  if (!read_line(up[0], &line, 20.0) || line.compare(0, 5, "port "))
  {
    res.why = "the room didn't start: " + line;
    write_line(down[1], "quit");
    waitpid(pid, nullptr, 0);
    close(down[1]);
    close(up[0]);
    return res;
  }
  const int port = atoi(line.c_str() + 5);
  JNL::open_socketlib();
  {
    Player me;
    me.setup(kbps, 1);
    me.client.Connect(("127.0.0.1:" + std::to_string(port)).c_str(), "anonymous:p1", "");
    me.start(rate, block, seconds + 5);
    // until each of the others plays here (their channel's meter moves), and one more interval
    auto all_heard = [&]() {
      if (me.client.GetStatus() != NJClient::NJC_STATUS_OK || me.client.GetNumUsers() < others) return false;
      int heard = 0;
      for (int u = 0; u < me.client.GetNumUsers(); u++)
        if (me.client.GetUserChannelPeak(u, 0) > 0.01f) heard++;
      return heard >= others;
    };
    const double t0 = mono_seconds();
    bool ok = false;
    while (mono_seconds() - t0 < 40)
    {
      if (all_heard())
      {
        ok = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!ok) res.why = "not every player heard after 40 s";
    else
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(4500)); // one interval more
      double up0, down0, up1, down1;
      tcp_bytes(&up0, &down0);
      const int64_t a0 = task_cpu_ns(me.audioTid.load()), n0 = task_cpu_ns(me.netTid.load());
      const double w0 = mono_seconds();
      me.recording.store(true);
      while (mono_seconds() - w0 < seconds) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      me.recording.store(false);
      const double w1 = mono_seconds();
      const int64_t a1 = task_cpu_ns(me.audioTid.load()), n1 = task_cpu_ns(me.netTid.load());
      tcp_bytes(&up1, &down1);
      res.seconds = w1 - w0;
      res.audioCpuPct = a0 >= 0 && a1 >= 0 ? 100.0 * (double)(a1 - a0) * 1e-9 / res.seconds : -1;
      res.netCpuPct = n0 >= 0 && n1 >= 0 ? 100.0 * (double)(n1 - n0) * 1e-9 / res.seconds : -1;
      res.upBytes = up1 - up0;
      res.downBytes = down1 - down0;
      const size_t k = me.count.load(std::memory_order_acquire);
      for (size_t i = 0; i < k; i++)
      {
        res.wall.push_back(me.wallUs[i]);
        res.cpu.push_back(me.cpuUs[i]);
        res.late.push_back(me.lateUs[i]);
      }
      res.audioRt = me.audioRt.load();
      res.users = me.client.GetNumUsers();
      res.ok = all_heard();
      if (!res.ok) res.why = "a player was not heard at the end";
    }
    me.stop(); // before Disconnect(), as in run_room()
    me.client.Disconnect();
  }
  JNL::close_socketlib();
  write_line(down[1], "quit");
  waitpid(pid, nullptr, 0);
  close(down[1]);
  close(up[0]);
  return res;
}

static int mode_cpu(const Args &a)
{
  const std::vector<double> playersList = a.nums("players", "1,3,5,7");
  const std::vector<double> blocks = a.nums("blocks", "64,128,256,512");
  const std::vector<double> kbpsList = a.nums("kbps", "510");
  const double seconds = a.num("seconds", 20), rate = a.num("rate", 48000);
  const int reps = (int)a.num("reps", 3);
  Csv out(a.str("out", "-"));
  const char *header = "block,players,rep,rate,kbps,seconds,blocks,block_us,wall_us_med,wall_us_p90,wall_us_p99,"
                       "wall_us_p999,wall_us_max,wall_us_mean,cpu_us_med,cpu_us_p99,cpu_us_mean,pct_of_block_med,"
                       "pct_of_block_mean,pct_of_block_p99,late_us_med,late_us_max,audio_thread_cpu_pct,net_thread_cpu_pct,"
                       "audio_rt,up_payload_kbps,down_payload_kbps,up_wire_kbps_est,down_wire_kbps_est";
  out.line("%s", header);
  const int fields = (int)std::count(header, header + strlen(header), ',') + 1;
  for (double kbps : kbpsList)
    for (double b : blocks)
      for (double pl : playersList)
        for (int rep = 1; rep <= reps; rep++)
        {
          const int block = (int)b, others = (int)pl;
          RunResult r = run_once(others, rate, block, (int)kbps, seconds);
          if (!r.ok)
          {
            say("cpu kbps %.0f block %d players %d rep %d: %s\n", kbps, block, others, rep, r.why.c_str());
            out.line("%d,%d,%d,%.0f,%.0f,%.1f%s", block, others, rep, rate, kbps, seconds,
                     std::string((size_t)(fields - 6), ',').c_str());
            continue;
          }
          const double blockUs = 1e6 * block / rate;
          std::vector<double> pctBlock;
          for (double w : r.wall) pctBlock.push_back(100.0 * w / blockUs);
          const double s = r.seconds, kbit = 8.0 / 1000.0;
          const double upB = r.upBytes / s, downB = r.downBytes / s;
          // the internet's wire: 52 bytes of headers per 1448-byte segment, an ACK per two the other way
          const double upWire = upB + 52 * upB / 1448 + 52 * (downB / 1448) / 2;
          const double downWire = downB + 52 * downB / 1448 + 52 * (upB / 1448) / 2;
          out.line("%d,%d,%d,%.0f,%.0f,%.1f,%zu,%.1f,%.2f,%.2f,%.2f,%.2f,%.1f,%.2f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.1f,%.1f,"
                   "%.2f,%.2f,%d,%.1f,%.1f,%.1f,%.1f",
                   block, others, rep, rate, kbps, s, r.wall.size(), blockUs, pct(r.wall, 50), pct(r.wall, 90), pct(r.wall, 99),
                   pct(r.wall, 99.9), pct(r.wall, 100), mean(r.wall), pct(r.cpu, 50), pct(r.cpu, 99), mean(r.cpu),
                   pct(pctBlock, 50), mean(pctBlock), pct(pctBlock, 99), pct(r.late, 50), pct(r.late, 100), r.audioCpuPct,
                   r.netCpuPct, r.audioRt ? 1 : 0, upB * kbit, downB * kbit, upWire * kbit, downWire * kbit);
          say("cpu kbps %.0f block %d players %d rep %d: AudioProc %.1f us median (%.2f%% of the block), p99 %.1f us, "
              "max %.0f us; audio thread %.2f%%, network thread %.2f%% of a core; up %.1f kbit/s, down %.1f kbit/s\n",
              kbps, block, others, rep, pct(r.wall, 50), pct(pctBlock, 50), pct(r.wall, 99), pct(r.wall, 100),
              r.audioCpuPct, r.netCpuPct, upB * kbit, downB * kbit);
        }
  return 0;
}

int main(int argc, char **argv)
{
  signal(SIGPIPE, SIG_IGN);
  if (argc < 2 || strcmp(argv[1], "cpu"))
  {
    say("usage: njclient_bench cpu [--name value ...] (see njclient_bench.cpp)\n");
    return 2;
  }
  Args a(argc, argv);
  if (!a.ok) return 2;
  return mode_cpu(a);
}
