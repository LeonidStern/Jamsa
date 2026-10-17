/*
    NINJAM VST3 - njclient_live_test.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  NJClient's side of Live mode (njclient.h: config_live, the server's
  MESSAGE_SERVER_LIVE, SetUserLive() and LiveAudio_Callback), with the
  server's own code (ninjamsrv_embedded, Live on) on loopback.

  A sender plays a tone with some noise in it; five receivers hear it through
  intervals, all in step (one thread, the same blocks, the network settled
  between blocks), each with its players on output pairs of their own as in
  the plug-in:
    r0  config_live off: NJClient as it was before Live
    r1  config_live on, no LiveAudio_Callback, SetUserLive() to a slot
    r2  config_live on, a callback with audio, SetUserLive() never called
    r3  config_live on, SetUserLive() to a slot, the callback says no
    r4  config_live on, SetUserLive() to a slot for three intervals, its
        callback giving known audio
  r1, r2 and r3 play exactly what r0 plays, bit for bit, on every output:
  Live that isn't playing changes nothing. r4 plays r0's audio until the
  interval after SetUserLive(), then the callback's audio in place of the
  sender's intervals, with the volume, pan, mute and output channels as
  NJClient's own mix has them, the meter showing it; when its callback says no
  in the middle of an interval it plays r0's audio again at once (the
  interval played on muted underneath), and from the interval after
  SetUserLive(-1) on it plays r0's audio again. Live again from interval 7
  on, StopUserLive() in it plays r0's audio again at once, and the slot asked
  for plays again from the next interval.

  And what the server says: the Live fields and the other Live players
  (not the sender and r0, which can't play Live), the room's Live turned on
  by a chat (!live on), and none of it after Disconnect().

*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#include "njclient.h"
#include "mpb.h"
#include "ninjamsrv_embedded.h"

static int g_failures = 0, g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; if (!((a) == (b))) { g_failures++; printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

static const int SRATE = 48000, BLOCK = 512, NCH = 18; // the plug-in's outputs: main and 8 player pairs
static const double PI = 3.14159265358979323846;

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// what the sender plays at sample n
static float sender_signal(long n)
{
  unsigned int r = (unsigned int)n * 2654435761u;
  r ^= r >> 13;
  r *= 0x5bd1e995u;
  r ^= r >> 15;
  const float noise = (float)((r & 0xffff) / 32768.0 - 1.0);
  return (float)(0.4 * sin(2 * PI * 440 * n / SRATE)) + 0.1f * noise;
}

// the Live audio r4's callback gives, at sample n
static float stem_left(long n) { return (float)(0.3 * sin(2 * PI * 330 * n / SRATE)); }
static float stem_right(long n) { return (float)(0.2 * sin(2 * PI * 550 * n / SRATE)); }

struct Receiver
{
  enum Callback { NONE, AUDIO, REFUSE };
  Receiver(const char *n, bool live, Callback cb) : name(n), callback(cb)
  {
    client.config_live = live;
    client.config_metronome = 0.0f;
    client.config_metronome_mute = true;
    client.config_remote_autochan = 2; // each player on an output pair of their own
    client.config_remote_autochan_nch = NCH;
    if (cb != NONE)
    {
      client.LiveAudio_Callback = liveAudio;
      client.LiveAudio_User = this;
    }
    for (int c = 0; c < NCH; c ++)
    {
      buf[c].assign(BLOCK, 0.0f);
      out[c] = buf[c].data();
    }
    left.assign(BLOCK, 0.0f);
    right.assign(BLOCK, 0.0f);
  }

  static bool liveAudio(void *user, int slot, int offset, int len, const float **l, const float **r)
  {
    Receiver *me = (Receiver *)user;
    me->calls++;
    if (slot != 0 || offset < 0 || len <= 0 || offset + len > BLOCK) me->badCalls++;
    if (me->callback != AUDIO || me->refuse) return false;
    *l = me->left.data() + offset;
    *r = me->right.data() + offset;
    return true;
  }

  void block(long pos, float **in)
  {
    for (int i = 0; i < BLOCK; i ++)
    {
      left[i] = stem_left(pos + i);
      right[i] = stem_right(pos + i);
    }
    client.AudioProc(in, 2, out, NCH, BLOCK, SRATE);
    for (int c = 0; c < NCH; c ++) rec[c].insert(rec[c].end(), buf[c].begin(), buf[c].end());
  }

  // the sender's index in this client's users
  int sender()
  {
    for (int u = 0; u < client.GetNumUsers(); u ++)
    {
      const char *n = client.GetUserState(u);
      if (n && !strncmp(n, "sender@", 7)) return u;
    }
    return -1;
  }

  std::string name;
  Callback callback;
  bool refuse = false; // the callback says no for now
  NJClient client;
  std::vector<float> buf[NCH], left, right, rec[NCH];
  float *out[NCH];
  int calls = 0, badCalls = 0;
};

static void server_log(void *, const char *text)
{
  if (getenv("NJCLIENT_LIVE_TEST_VERBOSE")) printf("server: %s\n", text);
}

// starts the server (Live on, rooms start with it off) on the first free port from 2349
static int start_server(std::string *cfgPath)
{
  const char *t = getenv("TMPDIR");
#ifdef _WIN32
  if (!t || !*t) t = getenv("TEMP");
  const int pid = _getpid();
#else
  const int pid = (int)getpid();
#endif
  std::string dir = t && *t ? t : "/tmp";
  if (dir.back() != '/' && dir.back() != '\\') dir += "/";
  *cfgPath = dir + "njclient-live-test-" + std::to_string(pid) + ".cfg";
  for (int port = 2349; port < 2359; port ++)
  {
    FILE *fp = fopen(cfgPath->c_str(), "w");
    if (!fp) return -1;
    fprintf(fp, "Port %d\nMaxUsers 8\nMaxChannels 32 32\nAnonymousUsers multi\nAnonymousUsersCanChat yes\n"
                "AnonymousMaskIP no\nDefaultBPM 120\nDefaultBPI 2\nSetVotingThreshold 1\nAllowCodecs opus flac\n"
                "RequireCodec opus\nLive require\nLiveKbps 128\nLiveFrame 240\n", port);
    fclose(fp);
    const int r = ninjamsrv_embedded_start(cfgPath->c_str(), server_log, nullptr);
    if (r == NINJAMSRV_OK) return port;
    if (r != NINJAMSRV_LISTEN_ERROR) return -1;
  }
  return -1;
}

struct LivePeer
{
  std::string name;
  unsigned int id;
  int flags;
};

static std::vector<LivePeer> live_peers(NJClient &c)
{
  std::vector<LivePeer> v;
  for (int i = 0; ; i ++)
  {
    LivePeer p;
    const char *n = c.GetLivePeer(i, &p.id, &p.flags);
    if (!n) break;
    p.name = n;
    v.push_back(p);
  }
  return v;
}

static bool has_peer(const std::vector<LivePeer> &v, const char *prefix)
{
  for (auto &p : v)
    if (!strncmp(p.name.c_str(), prefix, strlen(prefix))) return true;
  return false;
}

int main()
{
  JNL::open_socketlib();
  std::string cfg;
  const int port = start_server(&cfg);
  CHECK(port > 0);
  if (port <= 0)
  {
    printf("could not start the server\n");
    return 1;
  }
  const std::string addr = "127.0.0.1:" + std::to_string(port);

  NJClient sender;
  sender.config_metronome = 0.0f;
  sender.config_metronome_mute = true;
  sender.SetLocalChannelInfo(0, "tone", true, 0, true, 128, true, true);
  sender.SetLocalChannelMonitoring(0, true, 0.0f, false, 0.0f, true, true, false, false);

  Receiver r0("r0", false, Receiver::NONE), r1("r1", true, Receiver::NONE), r2("r2", true, Receiver::AUDIO),
           r3("r3", true, Receiver::REFUSE), r4("r4", true, Receiver::AUDIO);
  Receiver *rx[] = { &r0, &r1, &r2, &r3, &r4 };
  const int NRX = 5;

  sender.Connect(addr.c_str(), "anonymous:sender", "");
  for (Receiver *r : rx) r->client.Connect(addr.c_str(), ("anonymous:" + r->name).c_str(), "");

  // the network settles: everyone reads what came, until no one has more
  auto pump = [&]() {
    for (int pass = 0; pass < 3; pass ++)
    {
      for (int i = 0; i < 50; i ++)
      {
        bool busy = !sender.Run();
        for (Receiver *r : rx) busy = !r->client.Run() || busy;
        if (!busy) break;
      }
      if (pass < 2) sleep_ms(1);
    }
  };
  auto until = [&](double seconds, std::function<bool()> pred) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end)
    {
      if (pred()) return true;
      pump();
      sleep_ms(2);
    }
    return pred();
  };

  // all in, each receiver sees the sender's channel, and the Live clients the server's Live
  const bool in = until(15.0, [&]() {
    if (sender.GetStatus() != NJClient::NJC_STATUS_OK) return false;
    for (Receiver *r : rx)
    {
      if (r->client.GetStatus() != NJClient::NJC_STATUS_OK || r->sender() < 0) return false;
      if (r->client.config_live && live_peers(r->client).size() < 3) return false;
    }
    return true;
  });
  CHECK(in);
  if (!in)
  {
    printf("the clients did not all join\n");
    ninjamsrv_embedded_stop();
    return 1;
  }

  // the same mix in every receiver: the sender at full volume on the first player pair (each
  // receiver's own pick depends on who it saw join first)
  const int outch = 2, monoch = 6;
  for (Receiver *r : rx)
    r->client.SetUserChannelState(r->sender(), 0, false, false, true, 1.0f, true, 0.0f, false, false, false, false, true, outch);

  // what the server says about Live, before the room turns it on
  CHECK_EQ(r0.client.GetLiveGeneration(), 0); // it didn't ask (no CLIENT_CAP_LIVE)
  CHECK_EQ(r0.client.GetNumLivePeers(), 0);
  for (int i = 1; i < NRX; i ++)
  {
    NJClient &c = rx[i]->client;
    CHECK(c.GetLiveGeneration() > 0);
    CHECK(!(c.GetLiveFlags() & mpb_server_live::LIVE_FLAG_ROOM_ON)); // rooms start with Live off
    CHECK(c.GetLiveFlags() & mpb_server_live::LIVE_FLAG_UDP_OPEN);
    CHECK(c.GetLiveUdpPort() == 0 || c.GetLiveUdpPort() == port);
    CHECK_EQ(c.GetServerPort(), port);
    CHECK_EQ(c.GetServerAddress(), (unsigned int)inet_addr("127.0.0.1"));
    CHECK_EQ(c.GetLiveKbps(), 128);
    CHECK_EQ(c.GetLiveFrameSamples(), 240);
    CHECK(c.GetLiveMaxPeers() >= 4);
    CHECK(c.GetLivePeerId() != 0);
    CHECK(c.GetLiveKey() != 0);
    // the other Live players: not itself, not the sender or r0
    const std::vector<LivePeer> v = live_peers(c);
    CHECK_EQ((int)v.size(), 3);
    CHECK_EQ(c.GetNumLivePeers(), 3);
    CHECK(!has_peer(v, (rx[i]->name + "@").c_str()));
    CHECK(!has_peer(v, "sender@"));
    CHECK(!has_peer(v, "r0@"));
    for (int j = 1; j < NRX; j ++)
      if (j != i) CHECK(has_peer(v, (rx[j]->name + "@").c_str()));
    for (auto &p : v) CHECK(p.id != 0 && p.id != c.GetLivePeerId());
    unsigned int id = 1;
    int flags = 1;
    CHECK(c.GetLivePeer(3, &id, &flags) == nullptr);
    CHECK(c.GetLivePeer(-1, &id, &flags) == nullptr);
  }
  // each its own id and key
  CHECK(r1.client.GetLivePeerId() != r2.client.GetLivePeerId());
  CHECK(r1.client.GetLiveKey() != r2.client.GetLiveKey());

  // a player turns the room's Live on: every Live client hears it
  const int gen = r2.client.GetLiveGeneration();
  r1.client.ChatMessage_Send("MSG", "!live on");
  CHECK(until(10.0, [&]() {
    for (int i = 1; i < NRX; i ++)
      if (!(rx[i]->client.GetLiveFlags() & mpb_server_live::LIVE_FLAG_ROOM_ON)) return false;
    return true;
  }));
  CHECK(r2.client.GetLiveGeneration() > gen);
  CHECK_EQ(r0.client.GetLiveGeneration(), 0);

  // the sender starts half an interval before the receivers, so an interval's upload never
  // starts at a receiver's interval start
  std::vector<float> sin0(BLOCK), sin1(BLOCK), zero(BLOCK, 0.0f), sout0(BLOCK), sout1(BLOCK);
  float *sin_[2] = { sin0.data(), sin1.data() }, *sout[2] = { sout0.data(), sout1.data() };
  float *rin[2] = { zero.data(), zero.data() };
  long spos = 0;
  auto sender_block = [&]() {
    for (int i = 0; i < BLOCK; i ++) sin0[i] = sin1[i] = sender_signal(spos + i);
    sender.AudioProc(sin_, 2, sout, 2, BLOCK, SRATE);
    spos += BLOCK;
  };
  const int len = 60 * SRATE * 2 / 120; // 120 BPM, 2 BPI: one second
  while (spos < len / 2)
  {
    sender_block();
    pump();
  }

  // the receivers, in step; intervals counted from their first block on
  const int u4 = r4.sender();
  {
    int oc = -1;
    float vol = 0;
    r4.client.GetUserChannelState(u4, 0, nullptr, &vol, nullptr, nullptr, nullptr, &oc);
    CHECK(oc == outch && vol == 1.0f);
  }

  // r4's settings, by block (they change between blocks); it plays live from the interval
  // after SetUserLive() to the one after SetUserLive(-1), and r0's audio when not
  enum Mode { AS_IS, MUTED, VOLPAN, MONO, REFUSED };
  std::vector<int> mode;
  std::vector<long> starts;         // where each interval starts (receivers' samples)
  long pos = 0;
  int lastPos = -1;
  const int INTERVALS_RUN = 8;
  const int perInterval = len / BLOCK;
  int liveSeen[3] = { -2, -2, -2 }; // r4's GetUserLive() in intervals 2, 4 and 7
  long stoppedAt = -1;              // where r4's StopUserLive() took effect
  float liveMeter = -1, r0Meter = -1;
  while ((int)starts.size() <= INTERVALS_RUN)
  {
    const int iv = (int)starts.size() - 1; // the interval this block starts in (-1 before the first)
    const int at = iv >= 0 ? (int)((pos - starts[iv]) / BLOCK) : 0; // blocks into it
    int m = AS_IS;
    if (iv == 4)
    {
      // r4's player changed while live: muted, then half volume and panned left, then mono on
      // another output, then as it was
      const int q = at * 5 / perInterval;
      m = q == 0 ? MUTED : q == 1 ? VOLPAN : q == 2 ? MONO : AS_IS;
      if (q == 0) r4.client.SetUserState(u4, false, 0, false, 0, true, true);
      else r4.client.SetUserState(u4, false, 0, false, 0, true, false);
      if (q == 1) r4.client.SetUserChannelState(u4, 0, false, false, true, 0.5f, true, -0.5f, false, false, false, false);
      else r4.client.SetUserChannelState(u4, 0, false, false, true, 1.0f, true, 0.0f, false, false, false, false);
      r4.client.SetUserChannelState(u4, 0, false, false, false, 0, false, 0, false, false, false, false, true,
                                    q == 2 ? (monoch | 1024) : outch);
    }
    if (iv == 5)
    {
      // its Live stops in the middle of the interval (the room's Live off, in the plug-in):
      // the interval, at once
      r4.refuse = at >= perInterval * 3 / 4;
      m = r4.refuse ? REFUSED : AS_IS;
    }
    if (iv >= 6) r4.refuse = false;
    // SetUserLive() in the middle of an interval: from the next on
    if (iv == 2 && at == perInterval / 2)
    {
      const int s = r1.sender();
      r1.client.SetUserLive(s, 0);
      r3.client.SetUserLive(r3.sender(), 0);
      r4.client.SetUserLive(u4, 0);
      r4.client.SetUserLive(u4, 0); // twice: the same
      r4.client.SetUserLive(99, 0); // no such user: nothing
      r4.client.SetUserLive(-1, 0);
      CHECK_EQ(r4.client.GetUserLive(u4), -1); // not yet
      CHECK_EQ(r4.client.GetUserLive(99), -1);
    }
    if (iv == 5 && at == perInterval / 2)
    {
      r1.client.SetUserLive(r1.sender(), -1);
      r3.client.SetUserLive(r3.sender(), -7); // any negative: intervals
      r4.client.SetUserLive(u4, -1);
      CHECK_EQ(r4.client.GetUserLive(u4), 0); // until the interval ends
    }
    // live again from interval 7 on, then stopped at once in it (in the plug-in: the room's
    // Live off, the slots handed out again)
    if (iv == 6 && at == perInterval / 2)
    {
      r4.client.SetUserLive(u4, 0);
      r4.client.StopUserLive(u4); // nothing plays live: nothing to stop
      r4.client.StopUserLive(99);
      r4.client.StopUserLive(-1);
      CHECK_EQ(r4.client.GetUserLive(u4), -1);
    }
    if (iv == 7 && at == perInterval / 4)
    {
      CHECK_EQ(r4.client.GetUserLive(u4), 0);
      r4.client.StopUserLive(u4);
      CHECK_EQ(r4.client.GetUserLive(u4), -1); // at once
      stoppedAt = pos;
    }
    if (at == perInterval / 2)
    {
      if (iv == 2) liveSeen[0] = r4.client.GetUserLive(u4);
      if (iv == 4) liveSeen[1] = r4.client.GetUserLive(u4);
      if (iv == 7) liveSeen[2] = r4.client.GetUserLive(u4);
      if (iv == 3)
      {
        liveMeter = r4.client.GetUserChannelPeak(u4, 0, 0);
        r0Meter = r0.client.GetUserChannelPeak(r0.sender(), 0, 0);
      }
    }
    mode.push_back(m);

    for (Receiver *r : rx) r->block(pos, rin);
    sender_block();
    pump();

    // the interval position: the same in every receiver, and where an interval starts
    int p = 0, l = 0;
    r0.client.GetPosition(&p, &l);
    for (Receiver *r : rx)
    {
      int q = 0, ql = 0;
      r->client.GetPosition(&q, &ql);
      CHECK(q == p && ql == l);
    }
    if (lastPos < 0 || p < lastPos + BLOCK) starts.push_back(pos + BLOCK - p);
    lastPos = p;
    pos += BLOCK;
  }
  CHECK_EQ(liveSeen[0], -1);
  CHECK_EQ(liveSeen[1], 0);
  CHECK_EQ(liveSeen[2], -1);
  CHECK_EQ(r4.client.GetUserLive(u4), 0); // what it asked for, from interval 8 on
  CHECK(stoppedAt > 0);
  CHECK_EQ(r1.client.GetUserLive(r1.sender()), -1);
  CHECK_EQ(r2.client.GetUserLive(r2.sender()), -1);
  CHECK_EQ(r0.client.GetUserLive(r0.sender()), -1);
  // the meter shows what plays: the Live audio's left peak (0.3), the interval's for r0
  CHECK(liveMeter > 0.25f && liveMeter < 0.301f);
  CHECK(r0Meter > 0.35f);
  printf("meters: r4 live %.4f, r0 intervals %.4f\n", liveMeter, r0Meter);
  // the callbacks: only while live, with sane arguments; r1 has none, r2 never live
  CHECK(r4.calls > 0 && r3.calls > 0);
  CHECK_EQ(r2.calls, 0);
  CHECK_EQ(r4.badCalls, 0);
  CHECK_EQ(r3.badCalls, 0);

  const long total = (long)r0.rec[0].size();
  CHECK(starts.size() == (size_t)INTERVALS_RUN + 1 && starts[0] == 0);
  CHECK(labs(starts[1] - starts[0] - len) <= BLOCK && labs(starts[4] - starts[3] - len) <= BLOCK);

  // r0 hears the sender (or this would prove little)
  auto rms = [&](const std::vector<float> &v, long from, long to) {
    double e = 0;
    for (long i = from; i < to; i ++) e += (double)v[i] * v[i];
    return to > from ? sqrt(e / (to - from)) : 0.0;
  };
  printf("r0's player output: rms %.4f %.4f %.4f in intervals 2, 4, 7 (outputs %d/%d)\n",
         rms(r0.rec[outch], starts[2], starts[3]), rms(r0.rec[outch], starts[4], starts[5]),
         rms(r0.rec[outch], starts[7], total), outch, outch + 1);
  CHECK(rms(r0.rec[outch], starts[2], starts[3]) > 0.1);
  CHECK(rms(r0.rec[outch], starts[4], starts[5]) > 0.1);
  CHECK(rms(r0.rec[outch], starts[7], total) > 0.1);
  CHECK(rms(r0.rec[0], 0, total) == 0.0); // the main pair: the plug-in mixes it (no metronome here)

  // r1, r2, r3: exactly r0, on every output
  for (int i = 1; i <= 3; i ++)
  {
    bool same = true;
    for (int c = 0; c < NCH; c ++)
      if (rx[i]->rec[c].size() != r0.rec[c].size() ||
          memcmp(rx[i]->rec[c].data(), r0.rec[c].data(), r0.rec[c].size() * sizeof(float)))
      {
        same = false;
        printf("%s differs from r0 on output %d\n", rx[i]->name.c_str(), c);
      }
    CHECK(same);
  }

  // r4: r0's audio, or the callback's in its place
  long wrong = 0, firstWrong = -1;
  for (long i = 0; i < total; i ++)
  {
    const int m = mode[(size_t)(i / BLOCK)];
    const bool live = (i >= starts[3] && i < starts[6] && m != REFUSED) || (i >= starts[7] && i < stoppedAt) || i >= starts[8];
    for (int c = 0; c < NCH; c ++)
    {
      float want = r0.rec[c][(size_t)i];
      if (live)
      {
        const double sl = stem_left(i), sr = stem_right(i);
        want = 0.0f;
        if (m == AS_IS && c == outch) want = (float)sl;
        if (m == AS_IS && c == outch + 1) want = (float)sr;
        if (m == VOLPAN && c == outch) want = (float)(sl * 0.5);           // vol 0.5
        if (m == VOLPAN && c == outch + 1) want = (float)(sr * 0.5 * 0.5); // and panned left
        if (m == MONO && c == monoch) want = 0.0f + (float)(sl * 0.5) + (float)(sr * 0.5);
        // (MUTED: nothing anywhere)
      }
      if (r4.rec[c][(size_t)i] != want)
      {
        if (!wrong++) firstWrong = i;
        if (wrong <= 5)
          printf("r4 output %d at %ld (interval %d, mode %d): %.7f, not %.7f\n", c, i,
                 (int)(std::upper_bound(starts.begin(), starts.end(), i) - starts.begin()) - 1, m,
                 r4.rec[c][(size_t)i], want);
      }
    }
  }
  CHECK_EQ(wrong, 0);
  if (wrong) printf("r4: %ld wrong samples, the first at %ld\n", wrong, firstWrong);

  // Disconnect(): no Live; the others hear one fewer Live player
  const int before = r2.client.GetNumLivePeers();
  r1.client.Disconnect();
  CHECK_EQ(r1.client.GetLiveGeneration(), 0);
  CHECK_EQ(r1.client.GetLiveFlags(), 0);
  CHECK_EQ(r1.client.GetLivePeerId(), 0u);
  CHECK_EQ(r1.client.GetNumLivePeers(), 0);
  CHECK(r1.client.GetLivePeer(0, nullptr, nullptr) == nullptr);
  CHECK(until(10.0, [&]() { return r2.client.GetNumLivePeers() == before - 1 && !has_peer(live_peers(r2.client), "r1@"); }));

  for (Receiver *r : rx) r->client.Disconnect();
  sender.Disconnect();
  ninjamsrv_embedded_stop();
  remove(cfg.c_str());
  JNL::close_socketlib();

  printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
