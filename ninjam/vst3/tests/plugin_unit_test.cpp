/*
    NINJAM VST3 - plugin_unit_test.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Tests for the parts of the plug-in that need no network or host: the saved
  state, reading the built-in server's port mapping messages, following the
  host (lining the intervals up with its bars, voting for its tempo, the
  editor's note on its tempo), the metronome's click, what the editor's
  controls show and take, the text boxes' Cut, Copy, Paste and Select All
  keys, and Ableton Link: the beat and phase math, when the blocks are heard
  (the calibration), keeping the room's tempo on Link, the texts about it, and
  one instance leading Link in real time while another in the same program
  can't.

  "plugin_unit_test clipboard" checks the clipboard instead: the Copy buttons'
  text, and the text a paste reads (on Linux, with an X server: ctest runs it
  with xvfb-run).

*/

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "click.h"
#include "engine.h"
#include "hostserver.h"
#include "hostsync.h"
#include "linksync.h"
#include "textkeys.h"
#include "uitext.h"

#if defined(__linux__)
#include <stdlib.h>
#include <xcb/xcb.h>
#include "clipboard.h"
#endif

#ifndef _WIN32
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace ninjam_vst3;

static int g_failures = 0, g_checks = 0;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { g_checks++; if (!((a) == (b))) { g_failures++; printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } } while (0)

static void test_state()
{
  Settings s;
  s.userName = "J\xc3\xb6rg \\ \"tab\"\there\nnewline";
  s.server = "studio.lan:2050";
  s.inputVolume = 0.25f;
  s.metronomeVolume = 1.5f;
  s.metronomeMute = true;
  s.monitor = false;
  s.transmit = false;
  s.editorWidth = 1200;
  s.editorHeight = 800;
  s.opusKbps = 510;
  s.remote["alice/0"].volume = 0.5f;
  s.remote["alice/0"].pan = -0.75f;
  s.remote["bob\tx/1"].mute = true;

  const std::string data = s.serialize();
  Settings r;
  CHECK(r.deserialize(data));
  CHECK_EQ(r.userName, s.userName);
  CHECK_EQ(r.server, s.server);
  CHECK(fabsf(r.inputVolume - 0.25f) < 1e-6f);
  CHECK(fabsf(r.metronomeVolume - 1.5f) < 1e-6f);
  CHECK(r.metronomeMute && !r.monitor && !r.transmit);
  CHECK(r.editorWidth == 1200 && r.editorHeight == 800);
  CHECK_EQ(r.opusKbps, 510);
  CHECK_EQ(r.remote.size(), (size_t)2);
  CHECK(fabsf(r.remote["alice/0"].pan + 0.75f) < 1e-6f);
  CHECK(r.remote["bob\tx/1"].mute);

  // values out of range are clamped, unknown keys skipped, and other data refused
  Settings c;
  CHECK(c.deserialize("NINJAM-VST3-STATE 1\ninput_volume=99000000\nmetronome_volume=-5\nfuture_key=1\nremote=x\t1\n"));
  CHECK(c.inputVolume == 4.0f && c.metronomeVolume == 0.0f && c.remote.empty());
  Settings d;
  d.userName = "keep";
  CHECK(!d.deserialize("garbage"));
  CHECK(!d.deserialize(""));
  CHECK_EQ(d.userName, "keep");

  // a state saved with defaults loads as defaults
  Settings e;
  CHECK(e.deserialize(Settings().serialize()));
  CHECK(e.monitor && e.transmit && e.inputVolume == 1.0f && e.editorWidth == 0);
  CHECK(Settings().metronomeMute); // the metronome starts off
  // the size of the editor before its 720 x 330 layout opens at the new default
  Settings older;
  CHECK(older.deserialize("NINJAM-VST3-STATE 1\neditor_size=960x640\n"));
  CHECK(older.editorWidth == 0 && older.editorHeight == 0);
  CHECK_EQ(e.opusKbps, 510);

  // the Opus bitrate is one of the choices: the nearest one, or the default for junk
  struct { const char *saved; int kbps; } bitrates[] = {
    { "64", 64 }, { "96", 96 }, { "160", 160 }, { "256", 256 }, { "510", 510 }, { "300", 256 }, { "999999999999", 510 },
    { "-5", 64 }, { "abc", 510 }, { "", 510 }, { "128x", 510 }, { "128", 128 },
  };
  for (auto &b : bitrates)
  {
    Settings k;
    CHECK(k.deserialize(std::string("NINJAM-VST3-STATE 1\nopus_kbps=") + b.saved + "\n"));
    CHECK_EQ(k.opusKbps, b.kbps);
  }
  // a state saved before the bitrate was a setting
  Settings old;
  CHECK(old.deserialize("NINJAM-VST3-STATE 1\nuser=x\n"));
  CHECK_EQ(old.opusKbps, 510);

  // Sync (following the host): on by default, and in states saved before it was a setting
  CHECK(Settings().followHostTempo);
  CHECK(old.followHostTempo);
  Settings off;
  off.followHostTempo = false;
  Settings offLoaded;
  CHECK(offLoaded.deserialize(off.serialize()));
  CHECK(!offLoaded.followHostTempo);
  CHECK(e.followHostTempo);
}

// Live: the jitter buffer is saved with the state (states from before load with the default);
// the `live` text command, and the /live and /help chat commands out of a room
static void test_live_commands()
{
  CHECK_EQ(Settings().liveBufferMs, 20);
  CHECK(Settings().serialize().find("\nlive_buffer_ms=20\n") != std::string::npos);
  Settings s;
  s.liveBufferMs = 35;
  Settings r;
  CHECK(r.deserialize(s.serialize()));
  CHECK_EQ(r.liveBufferMs, 35);
  struct { const char *saved; int ms; } buffers[] = {
    { "5", 5 }, { "200", 200 }, { "1", 5 }, { "0", 5 }, { "-3", 5 }, { "999", 200 }, { "99999999999999999999", 200 },
    { "abc", 20 }, { "", 20 }, { "30ms", 20 }, { "30", 30 },
  };
  for (auto &b : buffers)
  {
    Settings k;
    CHECK(k.deserialize(std::string("NINJAM-VST3-STATE 1\nlive_buffer_ms=") + b.saved + "\n"));
    CHECK_EQ(k.liveBufferMs, b.ms);
  }
  Settings old; // saved before Live
  CHECK(old.deserialize("NINJAM-VST3-STATE 1\nuser=x\nopus_kbps=128\n"));
  CHECK_EQ(old.liveBufferMs, 20);

  Engine en;
  en.start(); // its network thread
  auto has = [](const std::string &text, const std::string &what) { return text.find(what) != std::string::npos; };
  // the chat has `what` (the engine's thread writes it), within a few seconds
  auto chat_has = [&](const std::string &what) {
    for (int i = 0; i < 300; i ++)
    {
      if (has(en.command("chatlog"), what)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    printf("chat:\n%s", en.command("chatlog").c_str());
    return false;
  };
  auto status_has = [&](const std::string &what) {
    for (int i = 0; i < 300; i ++)
    {
      if (has(en.command("live status"), what)) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    printf("live status: %s", en.command("live status").c_str());
    return false;
  };
  CHECK(status_has(" buffer=20 "));
  const std::string st = en.command("live status");
  CHECK(st.compare(0, 9, "live=off ") == 0);
  CHECK(has(st, " offered=0 ") && has(st, " running=0 ") && has(st, " buffer=20 ") && has(st, " peers=0\n"));
  CHECK_EQ(st.find('\n'), st.size() - 1); // no players: one line

  CHECK_EQ(en.command("live buffer 30"), "ok");
  CHECK_EQ(en.getSettings().liveBufferMs, 30);
  CHECK(status_has(" buffer=30 "));
  for (const char *bad : { "live buffer 4", "live buffer 201", "live buffer x", "live buffer", "live buffer 30ms" })
    CHECK(en.command(bad).compare(0, 25, "error: live buffer N, N f") == 0);
  CHECK(en.command("live maybe").compare(0, 6, "error:") == 0);
  CHECK_EQ(en.getSettings().liveBufferMs, 30);
  CHECK(has(en.getSettings().serialize(), "\nlive_buffer_ms=30\n"));

  // Live is the room's: out of a room, nothing to turn on
  CHECK_EQ(en.command("live on"), "ok");
  CHECK(chat_has("Not connected: Live mode is the room's, so join a room first."));

  // the chat's /live buffer N, any case
  en.sendChat("/live buffer 45");
  CHECK(chat_has("Live's jitter buffer is 45 ms now (it was 30)."));
  CHECK_EQ(en.getSettings().liveBufferMs, 45);
  en.sendChat("/live buffer 2");
  CHECK(chat_has("/live buffer takes a number of ms from 5 to 200, such as /live buffer 30."));
  CHECK_EQ(en.getSettings().liveBufferMs, 45);
  en.sendChat("/LIVE Buffer 50");
  CHECK(chat_has("Live's jitter buffer is 50 ms now (it was 45)."));
  en.sendChat("/live buffer");
  CHECK(chat_has("Live's jitter buffer is 50 ms. Type /live buffer 30 to change it (5 to 200)."));
  {
    // any case: the answer comes once more for each
    const std::string answer = "Not connected. In a room with Live mode, /live shows how late you hear each Jamsa player.";
    auto count = [&]() {
      const std::string log = en.command("chatlog");
      int n = 0;
      for (size_t at = log.find(answer); at != std::string::npos; at = log.find(answer, at + 1)) n ++;
      return n;
    };
    int want = count();
    for (const char *cmd : { "/live", "/LIVE", " /Live " })
    {
      en.sendChat(cmd);
      want ++;
      int n = 0;
      for (int i = 0; i < 300 && (n = count()) < want; i ++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      CHECK_EQ(n, want);
    }
    CHECK(chat_has(answer + " Live's jitter buffer is 50 ms"));
  }
  en.sendChat("/live off");
  en.sendChat("/live sideways");
  CHECK(chat_has("Type /live to see how the Live players sound, or /live buffer 30 to set Live's jitter buffer (ms)."));
  // /help: what Live adds
  en.sendChat("/Help");
  CHECK(chat_has("/live (how the Live players sound), /live buffer 30 (Live's jitter buffer in ms)"));
  CHECK(chat_has("!live on|off (Live mode: the room's Jamsa players hear each other in real time)"));
  // anything else still needs a room
  en.sendChat("hello");
  CHECK(chat_has("Not connected, so nobody can read that."));
  // a state loaded later sets it too
  en.setSettings(old);
  CHECK_EQ(en.getSettings().liveBufferMs, 20);
  CHECK(status_has(" buffer=20 inuse=20 "));

  // with the host's long blocks, more: two blocks and 5 ms (a player's audio comes a block at a
  // time and plays a block at a time)
  CHECK_EQ(Engine::liveBufferFor(20, 256, 48000), 20);
  CHECK_EQ(Engine::liveBufferFor(20, 512, 48000), 27);
  CHECK_EQ(Engine::liveBufferFor(20, 512, 44100), 29);
  CHECK_EQ(Engine::liveBufferFor(20, 1024, 48000), 48);
  CHECK_EQ(Engine::liveBufferFor(40, 1024, 48000), 48);
  CHECK_EQ(Engine::liveBufferFor(60, 1024, 48000), 60);
  CHECK_EQ(Engine::liveBufferFor(20, 2048, 48000), 91);
  CHECK_EQ(Engine::liveBufferFor(20, 8192, 48000), 200);
  CHECK_EQ(Engine::liveBufferFor(20, 0, 48000), 20);
  CHECK_EQ(Engine::liveBufferFor(1, 0, 0), 5);
  {
    std::vector<float> l(1024, 0.0f), r(1024, 0.0f), ol(1024), orr(1024);
    float *in[2] = { l.data(), r.data() }, *out[2] = { ol.data(), orr.data() };
    en.process(in, 2, out, 2, 1024, nullptr, nullptr);
  }
  CHECK(status_has(" buffer=20 inuse=48 hostblock=1024 "));
  en.sendChat("/live buffer 30");
  CHECK(chat_has("Live's jitter buffer is 30 ms now (it was 20). Less is less late, more has fewer dropouts: type /live to see "
                 "them. Your DAW's audio buffer (1024 samples) needs at least 48 ms, so Live uses 48 ms."));
  // more than Live can use: said so (not "Live uses 200 ms")
  CHECK_EQ(Engine::liveBufferNeeded(8192, 48000), 347);
  CHECK_EQ(Engine::longestLiveBlock(48000), 4096);
  CHECK_EQ(Engine::longestLiveBlock(44100), 4096);
  CHECK_EQ(Engine::longestLiveBlock(96000), 8192);
  {
    std::vector<float> l(8192, 0.0f), r(8192, 0.0f), ol(8192), orr(8192);
    float *in[2] = { l.data(), r.data() }, *out[2] = { ol.data(), orr.data() };
    en.process(in, 2, out, 2, 8192, nullptr, nullptr);
  }
  CHECK(status_has(" buffer=30 inuse=200 hostblock=8192 "));
  en.sendChat("/live buffer 40");
  CHECK(chat_has("Live's jitter buffer is 40 ms now (it was 30). Less is less late, more has fewer dropouts: type /live to see "
                 "them. Your DAW's audio buffer (8192 samples) needs at least 347 ms, more than Live can use (200 ms), so "
                 "expect dropouts: 4096 samples or less plays well."));
  en.stop();
}

// The built-in server has Live (UDP on its TCP port number); when no port from 2049 has UDP free
// too, it runs without, and says so.
static void test_hostserver_live()
{
#ifndef _WIN32
  setenv("JAMSA_WHATSMYIP", "off", 1); // no "what's my IP" service on the internet
  std::vector<int> taken;
  for (int p = HostServer::FIRST_PORT; p < HostServer::FIRST_PORT + HostServer::PORTS_TO_TRY; p ++)
  {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a {};
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)p);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (fd >= 0 && !bind(fd, (sockaddr *)&a, sizeof(a))) taken.push_back(fd);
    else if (fd >= 0) close(fd); // something else has it: the server can't have it either
  }
  int owner = 0;
  std::string err;
  HostServer &hs = HostServer::instance();
  CHECK(hs.start(&owner, 128, &err));
  HostServer::State st = hs.state();
  CHECK(st.owner == &owner && !st.live);
  CHECK(st.port >= HostServer::FIRST_PORT && st.port < HostServer::FIRST_PORT + HostServer::PORTS_TO_TRY);
  unsigned long long serial = 0;
  bool told = false;
  for (const std::string &m : hs.messagesSince(&serial))
    if (m == "Live mode is off on this server: no port from 2049 to 2058 had UDP free as well as TCP (other programs use "
             "them), so the players hear each other through intervals only.") told = true;
  CHECK(told);
  hs.stop(&owner);
  for (int fd : taken) close(fd);

  // UDP free: Live
  CHECK(hs.start(&owner, 128, &err));
  CHECK(hs.state().live);
  hs.stop(&owner);
  CHECK(hs.state().owner == nullptr);
  unsetenv("JAMSA_WHATSMYIP");
#endif
}

static void test_portmap_messages()
{
  HostServer::State st;
  bool show = false;
  HostServer::parseLogLine("[portmap] asking the router to forward TCP port 2049 to this machine", st, &show);
  CHECK(show && st.portmapPending && st.publicAddress.empty());

  HostServer::parseLogLine("[portmap] UPnP: people can connect to 203.0.113.7:2049", st, &show);
  CHECK(show && !st.portmapPending);
  CHECK_EQ(st.publicAddress, "203.0.113.7:2049");

  HostServer::parseLogLine("[portmap] lost the forward for TCP port 2049, trying again", st, &show);
  CHECK(st.publicAddress.empty() && st.portmapPending);

  HostServer::parseLogLine("[portmap] NAT-PMP: router forwards TCP port 2049 to this machine, but the router's own internet address is 100.64.0.9, "
                           "which is not public. Your internet provider probably uses carrier-grade NAT.", st, &show);
  CHECK(st.publicAddress.empty() && !st.portmapPending);
  CHECK(st.portmapNote.find("not public") != std::string::npos);
  CHECK(st.portmapNote.compare(0, 8, "NAT-PMP:") == 0);

  HostServer::parseLogLine("[portmap] UPnP: removed the forward for TCP port 2049", st, &show);
  CHECK(st.publicAddress.empty() && !st.portmapPending);

  HostServer::parseLogLine("Error listening on port 2049!", st, &show);
  CHECK(!show);
  HostServer::parseLogLine("User alice@1.2.3.4 connected", st, &show);
  CHECK(!show);
  HostServer::parseLogLine("Warning: could not open the log file", st, &show);
  CHECK(show);

  // a player who joins from the internet shows the port is open, one from the local network doesn't
  CHECK(!st.joinedFromInternet);
  HostServer::parseLogLine("192.168.1.20: Accepted user: Ana", st, &show);
  CHECK(!st.joinedFromInternet && !show);
  HostServer::parseLogLine("127.0.0.1: Accepted user: Ana", st, &show);
  CHECK(!st.joinedFromInternet);
  HostServer::parseLogLine("203.0.113.9: Refusing user Ana, invalid login/password", st, &show);
  CHECK(!st.joinedFromInternet);
  HostServer::parseLogLine("203.0.113.9: Accepted user: Ana", st, &show);
  CHECK(st.joinedFromInternet && !show);

  CHECK(HostServer::isPublicIPv4("89.114.98.123"));
  CHECK(HostServer::isPublicIPv4("172.32.0.1"));
  CHECK(HostServer::isPublicIPv4("100.128.0.1"));
  for (const char *a : { "10.0.0.5", "172.16.0.1", "172.31.255.255", "192.168.1.1", "127.0.0.1", "169.254.3.4",
                         "100.64.0.9", "100.127.255.1", "0.0.0.0", "224.0.0.1", "255.255.255.255", "", "1.2.3", "a.b.c.d" })
    CHECK(!HostServer::isPublicIPv4(a));

  // the public address from a "what's my IP" service
  std::string ip;
  CHECK(HostServer::parseWhatsMyIpReply("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n89.114.98.123\n", &ip));
  CHECK_EQ(ip, "89.114.98.123");
  CHECK(HostServer::parseWhatsMyIpReply("HTTP/1.0 200 OK\r\n\r\n203.0.113.7", &ip));
  CHECK_EQ(ip, "203.0.113.7");
  ip = "kept";
  CHECK(!HostServer::parseWhatsMyIpReply("HTTP/1.1 503 Service Unavailable\r\n\r\n1.2.3.4\n", &ip));
  CHECK(!HostServer::parseWhatsMyIpReply("HTTP/1.1 200 OK\r\n\r\n<html>1.2.3.4</html>", &ip));
  CHECK(!HostServer::parseWhatsMyIpReply("HTTP/1.1 200 OK\r\n\r\n1.2.3.256\n", &ip));
  CHECK(!HostServer::parseWhatsMyIpReply("HTTP/1.1 200 OK\r\n\r\n1.2.3\n", &ip));
  CHECK(!HostServer::parseWhatsMyIpReply("HTTP/1.1 200 OK\r\n\r\n2001:db8::1\n", &ip));
  CHECK(!HostServer::parseWhatsMyIpReply("HTTP/1.1 200 OK\r\n", &ip));
  CHECK(!HostServer::parseWhatsMyIpReply("", &ip));
  CHECK_EQ(ip, "kept");
}

static bool near(double a, double b, double eps = 1e-9) { return fabs(a - b) <= eps; }

static void test_bar_math()
{
  using namespace barmath;
  CHECK(near(hostIntervalPos(0, 4, 96000), 0));
  CHECK(near(hostIntervalPos(1, 4, 96000), 24000));
  CHECK(near(hostIntervalPos(9.5, 4, 96000), 36000)); // bar 3, beat 2 and a half
  CHECK(near(hostIntervalPos(-1, 4, 96000), 72000));  // before bar 1 (a count-in)
  CHECK(near(hostIntervalPos(16, 16, 96000), 0));
  CHECK(near(hostIntervalPos(5, 0, 96000), 0));

  CHECK(near(wrapSamples(100, 1000), 100));
  CHECK(near(wrapSamples(900, 1000), -100));
  CHECK(near(wrapSamples(-900, 1000), 100));
  CHECK(near(wrapSamples(500, 1000), -500)); // [-len/2, len/2)

  // the interval playing now ends when the host's next one starts, or the one after,
  // whichever is closer to the usual length
  CHECK_EQ(intervalEnd(1000, 96000, 1000), 95000);    // in line: the usual end
  CHECK_EQ(intervalEnd(1000, 96000, 1500), 94500);    // behind the host: 500 shorter
  CHECK_EQ(intervalEnd(1500, 96000, 1000), 95000);    // ahead of it: 500 longer (1500 + 95000)
  CHECK_EQ(intervalEnd(10, 96000, 95990), 96010);     // the host's next start is 10 samples away: 20 longer instead
  CHECK_EQ(intervalEnd(90000, 96000, 5000), 91000);   // the host's interval just started: 85000 longer
  CHECK_EQ(intervalEnd(95999, 96000, 95999.6), 96000); // less than a sample to the host's start: the one after
  CHECK(intervalEnd(0, 96000, 0) >= 1);
}

// NJClient's intervals, with NJClient::SetIntervalEnd() as it does it
struct Intervals
{
  int len, cur, pos, count = 0;
  long now = 0;              // samples played
  std::vector<long> starts;  // when each interval started
  Intervals(int len, int pos) : len(len), cur(len), pos(pos) {}
  void setEnd(int samples) { cur = pos + samples; }
  void play(int frames)
  {
    for (int i = 0; i < frames; i ++, now ++)
    {
      if (pos >= cur)
      {
        pos = 0;
        cur = len;
        count++;
        starts.push_back(now);
      }
      pos++;
    }
  }
};

// a host at a tempo, playing (or not) from a place, maybe in a loop
struct Host
{
  HostPosition p;
  double srate = 48000;
  bool hideCycle = false; // loops without saying so: the plug-in doesn't see p.cycle*
  double wobbleMs = 0;    // the position it gives is off by up to this much, at random
  unsigned seed = 1;
  void play(int frames)
  {
    if (!p.playing) return;
    p.ppq += frames / srate * p.tempo / 60;
    if (p.cycleActive && p.ppq >= p.cycleEnd) p.ppq -= p.cycleEnd - p.cycleStart;
  }
  // what the plug-in is told
  HostPosition told()
  {
    HostPosition t = p;
    if (hideCycle) t.cycleActive = false;
    if (wobbleMs > 0)
    {
      seed = seed * 1103515245u + 12345u;
      const double r = ((seed >> 8) & 0xffff) / 65535.0 * 2 - 1; // -1 .. 1
      t.ppq += r * wobbleMs / 60000.0 * p.tempo;
    }
    return t;
  }
};

// runs `seconds` of blocks through BarSync; returns the interval starts (samples) and checks
// each one that started after `alignedFrom` sits where the host is at a multiple of bpi beats
struct Run
{
  BarSync sync;
  Host host;
  Intervals iv;
  double bpm;
  int bpi, frames = 512;
  double hostBeatsAtMove = 0; long moveSample = 0; // to find where the host is, without its loops
  Run(double bpm, int bpi, int startPos)
    : iv((int)((double)bpi / (bpm / 60.0) * 48000), startPos), bpm(bpm), bpi(bpi) {}
  void move(double ppq)
  {
    host.p.ppq = ppq;
    hostBeatsAtMove = ppq;
    moveSample = iv.now;
  }
  // the host's beat at sample n, as if it never looped
  double beatAt(long n) const { return hostBeatsAtMove + (n - moveSample) / host.srate * host.p.tempo / 60; }
  void run(double seconds)
  {
    for (long n = 0; n < seconds * host.srate / frames; n ++)
    {
      BarSync::Interval i;
      i.running = true;
      i.pos = iv.pos;
      i.len = iv.len;
      i.bpi = bpi;
      i.bpm = bpm;
      i.count = iv.count;
      const int end = sync.process(host.told(), frames, host.srate, i);
      if (end > 0) iv.setEnd(end);
      iv.play(frames);
      const double was = host.p.ppq;
      host.play(frames);
      if (host.hideCycle && host.p.ppq < was && !wrapped)
      {
        // the first time round a loop the host doesn't declare is a move for the plug-in
        wrapped = true;
        hostBeatsAtMove = host.p.ppq;
        moveSample = iv.now;
      }
    }
  }
  bool wrapped = false;
  // the largest distance (ms) from a host interval start, of the interval starts after sample `from`
  double worstMs(long from) const
  {
    double worst = 0;
    for (long st : iv.starts)
      if (st >= from)
      {
        const double b = fmod(beatAt(st), bpi);
        const double off = std::min(b, bpi - b) * 60000.0 / host.p.tempo;
        worst = std::max(worst, off);
      }
    return worst;
  }
  int startsAfter(long from) const { return (int)std::count_if(iv.starts.begin(), iv.starts.end(), [&](long s) { return s >= from; }); }
  // are the intervals that started after `from` all of the usual length?
  bool regular(long from) const { return odd(from) == 0; }
  // how many intervals that started after `from` were of another length (what the others hear)
  int odd(long from = 0) const
  {
    int n = 0;
    for (size_t k = 1; k < iv.starts.size(); k ++)
      if (iv.starts[k - 1] >= from && iv.starts[k] - iv.starts[k - 1] != iv.len) n++;
    return n;
  }
};

static void test_bar_sync()
{
  // The host starts playing in the middle of bar 3, at the room's tempo: the interval playing
  // then ends early or late, and from the next one on they start on the host's bars.
  {
    Run r(100, 4, 30000);
    r.host.p.hasPosition = true;
    r.host.p.tempo = 100;
    r.run(3); // stopped: nothing moves
    CHECK(r.sync.state() == BarSync::Stopped);
    CHECK_EQ(r.sync.corrections(), 0);
    CHECK(r.regular(0));
    r.host.p.playing = true;
    r.move(9.37);
    const long start = r.iv.now;
    r.run(0.1);
    CHECK(r.sync.state() == BarSync::Lining);
    r.run(30);
    CHECK(r.sync.state() == BarSync::Aligned);
    CHECK_EQ(r.sync.corrections(), 1);
    const long firstInLine = r.iv.starts.empty() ? 0 : *std::find_if(r.iv.starts.begin(), r.iv.starts.end(), [&](long s) { return s > start; });
    CHECK(firstInLine - start <= (long)(r.iv.len * 1.5)); // within an interval and a half
    CHECK(r.startsAfter(firstInLine) >= 10);
    CHECK(r.worstMs(firstInLine) < 1.0 / 48); // within a sample
    CHECK(r.regular(firstInLine));
    printf("bars: in line %.2f s after the host started, worst %.4f ms\n", (firstInLine - start) / 48000.0, r.worstMs(firstInLine));

    // the host loops over 6 beats (not a multiple of BPI): the intervals play on as they were
    const int fixes = r.sync.corrections();
    r.host.p.cycleActive = true;
    r.host.p.cycleStart = 40;
    r.host.p.cycleEnd = 46;
    r.move(40);
    r.run(0.05);
    CHECK_EQ(r.sync.corrections(), fixes + 1); // moved into the loop: in line from there
    const long looping = r.iv.now;
    r.run(40); // more than ten loops
    CHECK_EQ(r.sync.corrections(), fixes + 1);
    CHECK(r.regular(looping));
    CHECK(r.worstMs(looping + r.iv.len * 2) < 1.0 / 48);
    r.host.p.cycleActive = false;

    // the playhead moves: in line again from the next interval
    r.move(101.5);
    const long moved = r.iv.now;
    r.run(20);
    CHECK_EQ(r.sync.corrections(), fixes + 2);
    CHECK(r.sync.state() == BarSync::Aligned);
    const long next = *std::find_if(r.iv.starts.begin(), r.iv.starts.end(), [&](long s) { return s > moved; });
    CHECK(r.worstMs(next) < 1.0 / 48);

    // stopped: the intervals play on, as they are
    r.host.p.playing = false;
    const long stopped = r.iv.now;
    r.run(20);
    CHECK(r.sync.state() == BarSync::Stopped);
    CHECK_EQ(r.sync.corrections(), fixes + 2);
    CHECK(r.regular(stopped));

    // another tempo than the room's: left alone
    r.host.p.playing = true;
    r.host.p.tempo = 101;
    r.move(3);
    const long other = r.iv.now;
    r.run(20);
    CHECK(r.sync.state() == BarSync::OtherTempo);
    CHECK_EQ(r.sync.corrections(), fixes + 2);
    CHECK(r.regular(other));

    // the interval each fix shortens or lengthens is one the room hears: one for each, no other
    CHECK_EQ(r.odd(), r.sync.corrections());
  }

  // A loop the host doesn't declare (no cycle in the ProcessContext): the first time round
  // is a move (one fix, in line from there), then each time is the same jump back, a loop
  // (no fix, the intervals play on as they were).
  {
    Run r(100, 4, 30000);
    r.host.p.hasPosition = true;
    r.host.p.tempo = 100;
    r.run(3);
    r.host.p.playing = true;
    r.host.hideCycle = r.host.p.cycleActive = true;
    r.host.p.cycleStart = 40;
    r.host.p.cycleEnd = 46;
    r.move(37); // a beat before the loop
    r.run(0.1);
    CHECK_EQ(r.sync.corrections(), 1); // started
    r.run(5.6); // 9 beats to the loop's end
    CHECK(r.wrapped);
    CHECK_EQ(r.sync.corrections(), 2); // the first time round
    const long wrapped = r.moveSample;
    r.run(60); // sixteen times round
    CHECK_EQ(r.sync.corrections(), 2);
    const long next = *std::find_if(r.iv.starts.begin(), r.iv.starts.end(), [&](long s) { return s > wrapped; });
    CHECK(r.regular(next));
    CHECK(r.worstMs(next) < 1.0 / 48); // in line with the host as if it hadn't looped since the first time
    CHECK_EQ(r.odd(), 2);
    printf("bars: a loop the host doesn't declare, %d times round: %d fixes, %d intervals of another length\n",
           (int)(60 / (6 * 60.0 / 100)), r.sync.corrections(), r.odd());

    // a move to another place is a move again, not a loop
    r.host.hideCycle = r.host.p.cycleActive = false;
    r.move(101.5);
    r.run(10);
    CHECK_EQ(r.sync.corrections(), 3);
    CHECK_EQ(r.odd(), 3);
  }

  // A host whose position wobbles (up to 2 ms off, at random, less than a move): one fix
  // when it starts, then none, so the room doesn't get an interval of another length now and then.
  {
    Run r(100, 4, 30000);
    r.host.p.hasPosition = r.host.p.playing = true;
    r.host.p.tempo = 100;
    r.host.wobbleMs = 2.0;
    r.move(9.37);
    r.run(60 * 5);
    printf("bars: a host 2 ms off at random, 5 minutes: %d fixes, worst %.3f ms\n", r.sync.corrections(), r.worstMs(r.iv.len * 2));
    CHECK_EQ(r.sync.corrections(), 1);
    CHECK(r.worstMs(r.iv.len * 2) < 2.1);
  }

  // NJClient's interval is a whole number of samples (0.63 short of 16 beats at 95 BPM): it
  // drifts from the host's bars by about a sample an interval, and gets a sample-sized fix now
  // and then
  {
    Run r(95, 16, 0);
    r.host.p.hasPosition = r.host.p.playing = true;
    r.host.p.tempo = 95;
    r.move(0);
    r.run(15);
    const long from = r.iv.now;
    r.run(60 * 20); // twenty minutes
    printf("bars at 95 BPM, 16 BPI, 20 minutes: %d fixes, worst %.3f ms\n", r.sync.corrections(), r.worstMs(from));
    CHECK(r.sync.corrections() <= 3);
    CHECK(r.worstMs(from) <= BarSync::TOLERANCE_MS + 0.05);
    CHECK_EQ(r.odd(), r.sync.corrections()); // each one a single interval of another length
  }
}

static void test_tempo_follower()
{
  TempoFollower f;
  // not in a room yet: the host's tempo is known, and waits for one
  CHECK_EQ(f.update(0, 100, 0), 0);
  CHECK_EQ(f.update(2, 100, 0), 0);
  CHECK(f.waiting());
  // joins a room at 120: one vote for 100
  f.arm();
  CHECK_EQ(f.update(3, 100, 120), 100);
  CHECK_EQ(f.update(3.05, 100, 120), 0);
  CHECK_EQ(f.update(4, 100, 100), 0);
  // someone else changes the room: no vote back
  for (double t = 5; t < 30; t += 0.05) CHECK_EQ(f.update(t, 100, 90), 0);
  // the host's tempo is dragged to 109.6: one vote, for 110, a second after it stops
  int votes = 0;
  double when = 0;
  const double drag[] = { 104, 106.5, 108, 109.6 };
  for (int i = 0; i < 4; i ++)
    for (double t = 30 + i * 0.2; t < 30 + (i + 1) * 0.2 - 1e-9; t += 0.05)
      if (f.update(t, drag[i], 90)) votes++;
  for (double t = 30.8; t < 35; t += 0.05)
  {
    const int bpm = f.update(t, 109.6, 90);
    if (bpm)
    {
      votes++;
      when = t;
      CHECK_EQ(bpm, 110);
    }
  }
  CHECK_EQ(votes, 1);
  CHECK(when >= 30.6 + TempoFollower::QUIET - 1e-6 && when < 30.6 + TempoFollower::QUIET + 0.11);
  // the room doesn't take it: no vote again until the host's tempo changes
  for (double t = 35; t < 60; t += 0.05) CHECK_EQ(f.update(t, 109.6, 90), 0);
  // a tempo that rounds to the room's, or that no room can have: no vote
  for (double t = 60; t < 63; t += 0.05) CHECK_EQ(f.update(t, 90.3, 90), 0);
  for (double t = 63; t < 66; t += 0.05) CHECK_EQ(f.update(t, 30, 90), 0);
  for (double t = 66; t < 69; t += 0.05) CHECK_EQ(f.update(t, 420, 90), 0);
  // the host stops saying its tempo for a while: nothing
  for (double t = 69; t < 72; t += 0.05) CHECK_EQ(f.update(t, 0, 90), 0);
  // following again (the toggle): one vote for the host's tempo, at once, as it is still
  CHECK_EQ(f.update(72, 420, 90), 0);
  CHECK_EQ(f.update(74, 128, 90), 0);
  CHECK_EQ(f.update(75.5, 128, 90), 128);
  f.arm();
  CHECK_EQ(f.update(76, 128, 90), 128);
  CHECK_EQ(f.update(77, 128, 90), 0);
  CHECK_EQ(f.votes(), 4);
}

static void test_host_note()
{
  CHECK_EQ(shortHostName("Ableton Live 12 Suite"), "Live");
  CHECK_EQ(shortHostName(" Live "), "Live");
  CHECK_EQ(shortHostName("Bitwig Studio"), "Bitwig Studio");
  CHECK_EQ(shortHostName(""), "The DAW");
  CHECK_EQ(shortHostName("A very long host name that goes on and on"), "A very long host name that goes ");
  CHECK_EQ(shortHostName("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9x\xc3\xa9\xc3\xa9"),
           "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9x");

  CHECK_EQ(bpmText(120), "120");
  CHECK_EQ(bpmText(109.6), "109.6");
  CHECK_EQ(bpmText(120.25), "120.25");
  CHECK_EQ(hostTempoNote("Ableton Live 12 Suite", 120, 95, true), "Live is at 120 BPM, the room at 95: set Live to 95");
  CHECK_EQ(hostTempoNote("Ableton Live 12 Suite", 120, 95, false),
           "Live is at 120 BPM, the room at 95: set Live to 95, or turn on Sync");
  CHECK_EQ(hostTempoNote("Bitwig Studio", 95.3, 95, true), "Bitwig Studio is at 95.3 BPM, the room at 95: set its tempo to 95");
  CHECK_EQ(hostTempoNote("Ableton Live 12", 95.004, 95, true), "");
  CHECK_EQ(hostTempoNote("Ableton Live 12", 0, 95, true), "");
  CHECK_EQ(hostTempoNote("Ableton Live 12", 120, 0, true), "");
  // where Link would lead the host (Sync is on, and this instance leads Link or would)
  CHECK_EQ(hostTempoNote("Ableton Live 12 Suite", 120, 95, true, true), "Live is at 120 BPM, the room at 95: turn on Link in Live, or set it to 95");
  CHECK_EQ(hostTempoNote("Bitwig Studio", 120, 95, true, true),
           "Bitwig Studio is at 120 BPM, the room at 95: turn on its Link if it has one, or set its tempo to 95");
  CHECK_EQ(hostTempoNote("Ableton Live 12 Suite", 120, 95, false, true), "Live is at 120 BPM, the room at 95: set Live to 95, or turn on Sync");
  CHECK_EQ(hostTempoNote("Ableton Live 12", 95, 95, true, true), "");

  // the transport bar's name for the host
  CHECK_EQ(hostChipName("Ableton Live 12 Suite"), "Live");
  CHECK_EQ(hostChipName("REAPER"), "REAPER");
  CHECK_EQ(hostChipName("Bitwig Studio"), "Host");
  CHECK_EQ(hostChipName(""), "Host");
  CHECK_EQ(hostChipName("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9"), "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9");

  // where the next interval starts, in the host's bars
  CHECK_EQ(nextIntervalText("Live", 32, 4, 4), "Next interval at Live bar 9");
  CHECK_EQ(nextIntervalText("Live", 31.9999999, 0, 0), "Next interval at Live bar 9"); // 4/4 when the host doesn't say
  CHECK_EQ(nextIntervalText("Live", 34, 4, 4), "Next interval at Live bar 9, beat 3");
  CHECK_EQ(nextIntervalText("Host", 0, 3, 4), "Next interval at Host bar 1");
  CHECK_EQ(nextIntervalText("Live", 9, 6, 8), "Next interval at Live bar 4"); // 6/8: a bar is 3 quarter notes
  CHECK_EQ(nextIntervalText("Live", 10, 6, 8), "Next interval at Live bar 4, beat 3");
  CHECK_EQ(nextIntervalText("Live", 33.25, 4, 4), "Next interval at Live bar 9, beat 2");
  int bar = 0, beat = 0;
  bool onBeat = true;
  barAndBeat(33.25, 4, 4, &bar, &beat, &onBeat);
  CHECK(bar == 9 && beat == 2 && !onBeat);
}


// ---------------------------------------------------------------------------
// Ableton Link
// ---------------------------------------------------------------------------

static void test_link_math()
{
  using namespace linkmath;
  CHECK(near(intervalBeat(0, 96000, 4), 0));
  CHECK(near(intervalBeat(48000, 96000, 4), 2));
  CHECK(near(intervalBeat(96000, 96000, 4), 4)); // the next interval starts with this sample
  CHECK(near(intervalBeat(10, 0, 4), 0));

  CHECK(near(wrapPhase(0.1, 4), 0.1));
  CHECK(near(wrapPhase(3.9, 4), -0.1));
  CHECK(near(wrapPhase(-3.9, 4), 0.1));
  CHECK(near(wrapPhase(4.1, 4), 0.1));
  CHECK(near(wrapPhase(6.1, 4), -1.9));
  CHECK(near(wrapPhase(2.0, 4), -2.0)); // [-q/2, q/2)
  CHECK(near(wrapPhase(-2.0, 4), -2.0));
  CHECK(near(wrapPhase(15.999, 16), -0.001));
  CHECK(near(wrapPhase(0.75, 1), -0.25));
  CHECK(near(wrapPhase(1, 0), 0));

  CHECK(near(beatsToMs(1, 120), 500));
  CHECK(near(msToBeats(500, 120), 1));
  CHECK(near(beatsToMs(4, 95), 4 * 60000.0 / 95));

  // the room's interval start (beat 0) on Link: a jump to start with, or when the beat is far
  // off; small steps for small errors
  Correction c = phaseCorrection(msToBeats(0.1, 120), 120, true, 0);
  CHECK(c.kind == Correction::Jump && near(c.shift, msToBeats(0.1, 120)));
  c = phaseCorrection(msToBeats(-40, 120), 120, false, 1e9);
  CHECK(c.kind == Correction::Jump && near(c.shift, msToBeats(-40, 120)));
  c = phaseCorrection(msToBeats(0.3, 120), 120, false, 1e9);
  CHECK(c.kind == Correction::None);
  const double since = STEP_EVERY_MS + 1;
  c = phaseCorrection(msToBeats(5, 120), 120, false, since);
  CHECK(c.kind == Correction::Step && near(c.shift, msToBeats(STEP_MS, 120)));
  c = phaseCorrection(msToBeats(-5, 120), 120, false, since);
  CHECK(c.kind == Correction::Step && near(c.shift, -msToBeats(STEP_MS, 120)));
  c = phaseCorrection(msToBeats(0.8, 120), 120, false, since);
  CHECK(c.kind == Correction::Step && near(c.shift, msToBeats(0.8, 120)));
  c = phaseCorrection(msToBeats(5, 120), 120, false, STEP_EVERY_MS - 100); // the last step was too recent
  CHECK(c.kind == Correction::None);
  c = phaseCorrection(msToBeats(29, 120), 120, false, since);
  CHECK(c.kind == Correction::Step);
  // a step only once Link shows the plug-in the network's changes again, not its own last one
  CHECK(linkGraceMs() > 0 && STEP_EVERY_MS > linkGraceMs());

  // The audio clock runs fast or slow against Link's (by up to 0.1%): after the first jump,
  // steps keep the interval starts on Link's beats within a little more than a step, with no jump.
  for (double ppm : { 50.0, -300.0, 1000.0 })
  {
    const double bpm = 95, bpi = 16, blockMs = 512 / 48.0;
    double linkOffset = 1.2345; // Link's beat at time 0
    double sinceStep = 1e9, worst = 0;
    int jumps = 0, steps = 0;
    for (int i = 0; i < 60 * 1000 * 10 / blockMs; i ++) // ten minutes
    {
      const double t = i * blockMs / 1000.0;
      const double ours = fmod(t * (1 + ppm * 1e-6) * bpm / 60, bpi);
      const double link = linkOffset + t * bpm / 60;
      const double e = wrapPhase(ours - link, bpi);
      if (i > 0) worst = std::max(worst, fabs(beatsToMs(e, bpm)));
      const Correction k = phaseCorrection(e, bpm, i == 0, sinceStep);
      sinceStep += blockMs;
      if (k.kind == Correction::None) continue;
      linkOffset += k.shift;
      sinceStep = 0;
      (k.kind == Correction::Jump ? jumps : steps)++;
    }
    printf("drift %+.0f ppm: %d jump, %d steps, worst %.2f ms\n", ppm, jumps, steps, worst);
    CHECK_EQ(jumps, 1);
    CHECK(steps > 0);
    CHECK(worst < STEP_MS + DEADBAND_MS);
  }

  MoveCounter m;
  CHECK(!m.add(0));
  CHECK(!m.add(5));
  CHECK(m.add(10)); // three in 20 seconds
  m.reset();
  CHECK(!m.add(0));
  CHECK(!m.add(15));
  CHECK(!m.add(30)); // the first is too old
  CHECK(m.add(31));
  CHECK(m.add(32));
}

// When blocks start on Link's clock (BlockClock), from the times process() is called: calls that
// come late (a busy machine, a timer that wakes up late, the host catching up after a stall)
// don't move it; the sound card's clock drifting from Link's does, and so does the sample count
// losing time.
static void test_block_clock()
{
  using linkmath::BlockClock;
  const double srate = 48000, frames = 512, blockUs = frames * 1e6 / srate;
  const double t0 = 5e9; // Link's clock at sample 0
  unsigned int rng = 1;
  auto uniform = [&]() {
    rng = rng * 1103515245u + 12345u;
    return ((rng >> 8) & 0xffff) / 65536.0;
  };
  const long minute = (long)(60 * srate / frames);

  // calls 0.2 to 15 ms late (a 15.6 ms Windows timer), and 80 ms late every 2 seconds, then
  // back to back until the host catches up: the clock stays within 1 ms of the earliest calls
  // (the earliest of two seconds varies by about that here), far from a jump (JUMP_MS)
  {
    BlockClock c;
    double lo = 1e18, hi = -1e18, late = 0;
    for (long i = 0; i < minute; i ++)
    {
      const double nominal = i * blockUs;
      late = std::max(late - blockUs, 0.0) + 200 + 15000 * uniform(); // what the stall left, and this call's
      if (i % 188 == 100) late += 80000;
      const double off = c.update(nominal, t0 + nominal + late, blockUs) - t0;
      if (nominal >= 3e6)
      {
        lo = std::min(lo, off);
        hi = std::max(hi, off);
      }
    }
    printf("block clock, calls 0.2 to 15 ms late and stalls: %.3f to %.3f ms after the earliest\n", (lo - 200) / 1000,
           (hi - 200) / 1000);
    CHECK(lo >= 200 - 1 && hi - 200 < 1000);
  }

  // a sound card 100 ppm slow against Link's clock: it follows, a little behind (the window and
  // the smoothing: a lag that stays the same, which the Calibrator takes up with the latency)
  {
    BlockClock c;
    double worst = 0;
    for (long i = 0; i < minute; i ++)
    {
      const double nominal = i * blockUs, drift = nominal * 100e-6;
      const double off = c.update(nominal, t0 + nominal + drift + 200 + 2000 * uniform(), blockUs) - t0;
      if (nominal >= 3e6) worst = std::max(worst, fabs(off - drift - 200));
    }
    printf("block clock, a sound card 100 ppm slow: %.3f ms behind at most\n", worst / 1000);
    CHECK(worst < 400);
  }

  // the sample count loses 100 ms (the host dropped audio): there once the window is past
  {
    BlockClock c;
    double off = 0;
    long moved = -1;
    for (long i = 0; i < minute / 2; i ++)
    {
      const double nominal = i * blockUs;
      const double lost = i >= minute / 4 ? 100000 : 0;
      off = c.update(nominal, t0 + nominal + lost + 200 + 1000 * uniform(), blockUs) - t0;
      if (i < minute / 4) CHECK(off < 200 + 1000);
      else if (moved < 0 && off > 100000) moved = i;
    }
    CHECK(moved > 0 && (moved - minute / 4) * blockUs <= (BlockClock::WINDOW_MS + 300) * 1000);
    CHECK(fabs(off - 100000 - 200) < 1000);
  }

  // a first call 20 ms late doesn't stay; after reset() (a new sample rate) it starts again
  {
    BlockClock c;
    CHECK(near(c.update(0, t0 + 20000, blockUs) - t0, 20000));
    CHECK(near(c.update(blockUs, t0 + blockUs + 200, blockUs) - t0, 200));
    c.reset();
    CHECK(near(c.update(1e6, t0 + 3e6, blockUs) - t0, 2e6));
  }
}

// Rounds of blocks into a Calibrator, as LinkSync feeds it: `residual(offset)` is how far the
// host's beat is after Link's at the time the plug-in takes a block to be heard (its guess plus
// the offset). Returns the rounds that moved the offset.
template <typename F> static int calibrate(linkmath::Calibrator &c, int rounds, F residual, double noiseMs = 0.1)
{
  const double blockMs = 512 / 48.0;
  int moved = 0;
  unsigned int rng = 12345;
  for (int r = 0; r < rounds; r ++)
  {
    const double res = residual(c.offsetMs());
    for (double ms = 0; ms < linkmath::Calibrator::WINDOW_MS + blockMs; ms += blockMs)
    {
      rng = rng * 1103515245u + 12345u;
      const double noise = ((rng >> 16) % 1000 / 999.0 - 0.5) * 2 * noiseMs;
      if (c.add(res + noise, blockMs))
      {
        moved++;
        break; // LinkSync waits for Link to line up at the new time, and settle
      }
    }
  }
  return moved;
}

static void test_calibrator()
{
  using linkmath::Calibrator;
  // Live follows Link: its beat for a block is Link's when the block is really heard, 7.5 ms
  // after the plug-in's guess. One round measures it, the next ones find it in line.
  Calibrator c;
  CHECK_EQ(calibrate(c, 1, [](double offset) { return 7.5 - offset; }), 1);
  CHECK(fabs(c.offsetMs() - 7.5) < 0.2);
  CHECK_EQ(calibrate(c, 5, [](double offset) { return 7.5 - offset; }), 0);
  CHECK(!c.gaveUp() && c.rounds() == 1);
  // heard before the guess (a host that passes an early position): a negative offset
  Calibrator early;
  calibrate(early, 4, [](double offset) { return -3.0 - offset; });
  CHECK(fabs(early.offsetMs() + 3.0) < 0.2 && !early.gaveUp());
  // a host that doesn't quite follow at once (it catches up on Link's move by 80% when measured):
  // more rounds, still there
  Calibrator slow;
  double seen = 0;
  calibrate(slow, 8, [&](double offset) {
    seen += 0.8 * (offset - seen);
    return 12.0 - seen;
  });
  CHECK(fabs(slow.offsetMs() - 12.0) < 0.6 && !slow.gaveUp());
  printf("calibration: %.2f ms in %d rounds (a host that catches up on Link by 80%% when measured)\n", slow.offsetMs(), slow.rounds());
  // in line: nothing moves
  Calibrator none;
  CHECK_EQ(calibrate(none, 5, [](double) { return 0.1; }), 0);
  CHECK(near(none.offsetMs(), 0));

  // The host's beat doesn't follow Link (its own Link is off, and another app is the peer):
  // moving the time Link lines up at changes nothing, so the second round gives up, back to 0.
  Calibrator off;
  CHECK_EQ(calibrate(off, 2, [](double) { return 9.0; }), 2);
  CHECK(off.gaveUp());
  CHECK(near(off.offsetMs(), 0));
  CHECK_EQ(calibrate(off, 3, [](double) { return 9.0; }), 0); // until reset()
  off.reset();
  CHECK(!off.gaveUp());

  // a beat that moves within a round (a loop, the playhead): no round counts
  Calibrator jumpy;
  int moved = 0;
  for (int r = 0; r < 5; r ++)
    for (int i = 0; i < 100; i ++)
      if (jumpy.add(i < 50 ? 5.0 : 9.0, 512 / 48.0)) moved++;
  CHECK_EQ(moved, 0);
  CHECK(near(jumpy.offsetMs(), 0));
  // skipped blocks start the round again
  Calibrator skipped;
  for (int i = 0; i < 1000; i ++)
  {
    if (skipped.add(5.0, 512 / 48.0)) moved++;
    if (i % 50 == 49) skipped.skip();
  }
  CHECK_EQ(moved, 0);
  // never more than MAX_MS from the guess
  Calibrator far;
  calibrate(far, 1, [](double offset) { return 95.0 - offset; });
  CHECK(far.offsetMs() <= Calibrator::MAX_MS);
}

static void test_tempo_keeper()
{
  typedef LinkTempoKeeper::Action A;
  const double D = LinkTempoKeeper::DELAY + 0.05;
  // update() every 50 ms from `from` to `to` with the same inputs, until it acts (the plug-in
  // puts the room's tempo back on Link then): the action, with its time
  struct Took { double t; A a; };
  auto run = [](LinkTempoKeeper &k, double from, double to, double link, double room, int server, double host, double hostChanged,
                size_t peers = 1) {
    std::vector<Took> out;
    for (double t = from; t < to - 1e-9 && out.empty(); t += 0.05)
    {
      const A a = k.update(t, link, room, server, host, hostChanged, peers);
      if (a.kind != A::None) out.push_back({ t, a });
    }
    return out;
  };

  LinkTempoKeeper k;
  // Link has the room's tempo, and Live follows it: nothing
  CHECK(run(k, 0, 5, 120, 120, 120, 120, -100).empty());
  // no peers: nobody to lead
  CHECK(run(k, 5, 10, 133, 120, 120, 120, -100, 0).empty());

  // another app sets Link's tempo, and the host (whose Link is off) stays at the room's: Link
  // gets the room's back, DELAY later
  auto a = run(k, 10, 12, 133, 120, 120, 120, -100);
  CHECK(a.size() == 1 && a[0].a.why == A::Foreign && a[0].t >= 10 + LinkTempoKeeper::DELAY - 1e-6 && a[0].t < 10 + D);
  // the same when the host doesn't say its tempo
  a = run(k, 12, 14, 133, 120, 120, 0, -100);
  CHECK(a.size() == 1 && a[0].a.why == A::Foreign);

  // the plug-in joined Live's Link session, which has Live's tempo, the one Live has had all
  // along: the room's goes to Link (no vote)
  a = run(k, 40, 42, 133, 120, 120, 133, -100);
  CHECK(a.size() == 1 && a[0].a.why == A::Earlier);

  // the room's tempo changes (a vote passed): Link gets it at the next interval, so nothing
  CHECK(run(k, 42, 60, 120, 120, 100, 120, -100).empty());

  // Live's tempo is dragged to 130: nothing while it moves, nor while the vote for it waits
  LinkTempoKeeper v;
  CHECK(run(v, 0, 1, 130, 120, 120, 130, 0.9).empty());
  CHECK(run(v, 1, 2, 130, 120, 120, 130, 0.9).empty()); // still (the TempoFollower votes at 1.9)
  v.voted(1.9, 130);
  CHECK(run(v, 2, 1.9 + LinkTempoKeeper::VOTE_WAIT - 0.05, 130, 120, 120, 130, 0.9).empty());
  // the room takes it: Link gets it at the next interval
  CHECK(run(v, 6.85, 20, 130, 120, 130, 130, 0.9).empty());
  CHECK(run(v, 20, 25, 130, 130, 130, 130, 0.9).empty());
  CHECK_EQ(v.restores(), 0);
  // later Live goes to 130.3, the room's BPM: back to the room's (the vote for 130 was taken)
  a = run(v, 30, 33, 130.3, 130, 130, 130.3, 29.9);
  CHECK(a.size() == 1 && a[0].a.why == A::SameBpm);

  // a vote the room doesn't take: back to the room's VOTE_WAIT after it
  LinkTempoKeeper n;
  CHECK(run(n, 0, 1.5, 130, 120, 120, 130, 0.5).empty());
  n.voted(1.5, 130);
  a = run(n, 1.5, 10, 130, 120, 120, 130, 0.5);
  CHECK(a.size() == 1 && a[0].a.why == A::NotTaken);
  if (!a.empty()) CHECK(a[0].t >= 1.5 + LinkTempoKeeper::VOTE_WAIT - 1e-6 && a[0].t < 1.5 + LinkTempoKeeper::VOTE_WAIT + 0.06);
  // dragged again to the same tempo: a vote again (the TempoFollower), and the wait again
  n.voted(12, 130);
  CHECK(run(n, 12, 16.9, 130, 120, 120, 130, 11).empty());

  // a tempo that rounds to the room's, or one no room can have: no vote, back to the room's once
  // the host's tempo is still
  LinkTempoKeeper r;
  CHECK(run(r, 0, 1.2, 120.3, 120, 120, 120.3, 0).empty());
  a = run(r, 1.2, 3, 120.3, 120, 120, 120.3, 0);
  CHECK(a.size() == 1 && a[0].a.why == A::SameBpm && a[0].t >= LinkTempoKeeper::HOLD - 1e-6);
  a = run(r, 3, 6, 30, 120, 120, 30, 2.9);
  CHECK(a.size() == 1 && a[0].a.why == A::OutOfRange);
  a = run(r, 6, 9, 420, 120, 120, 420, 5.9);
  CHECK(a.size() == 1 && a[0].a.why == A::OutOfRange);
  CHECK_EQ(r.restores(), 3);
  CHECK(!r.contested());

  // something keeps putting its own tempo on Link: after FOREIGN_RESTORES restores in
  // FOREIGN_WINDOW seconds, the plug-in stops leading Link
  LinkTempoKeeper f;
  int restores = 0;
  for (int i = 0; i < 6; i ++)
  {
    const double t = i * 3.0;
    restores += (int)run(f, t, t + 1, 99, 120, 120, 120, -100).size();
    CHECK(run(f, t + 1, t + 3, 120, 120, 120, 120, -100).empty());
  }
  CHECK_EQ(restores, LinkTempoKeeper::FOREIGN_RESTORES);
  CHECK(f.contested());
  f.reset();
  CHECK(!f.contested());
  // spread out over more than the window, it's not a fight
  LinkTempoKeeper g;
  for (int i = 0; i < 6; i ++)
  {
    const double t = i * 12.0;
    CHECK_EQ(run(g, t, t + 1, 99, 120, 120, 120, -100).size(), (size_t)1);
  }
  CHECK(!g.contested());
}

static void test_link_texts()
{
  CHECK_EQ(linkHint("Ableton Live 12 Suite"), "Turn on Link in Live: then Live takes the room's tempo, and its bars start with the intervals.");
  CHECK_EQ(linkHint("Bitwig Studio"),
           "Turn on Link in Bitwig Studio if it has Link: then it takes the room's tempo, and its bars start with the intervals.");
  CHECK_EQ(linkHint(""), "Turn on Link in your DAW if it has Link: then it takes the room's tempo, and its bars start with the intervals.");

  const std::string live = "Ableton Live 12 Suite";
  const std::string plain = "Sync with Live: when you change its tempo, vote the room to it; and while it plays, start the "
                            "intervals on its bars (a multiple of BPI beats from bar 1)";
  CHECK_EQ(syncTooltip(live, LinkState::Off, 0), plain);
  CHECK_EQ(syncTooltip(live, LinkState::Leading, 1),
           "Sync with Live through Link: Live takes the room's tempo, and its bars start with the intervals; when you change "
           "Live's tempo, you vote the room to it");
  // out of a room (not in Link yet)
  CHECK_EQ(syncTooltip(live, LinkState::Waiting, 0),
           plain + ". In a room, with Link on in Live, Live takes the room's tempo instead, and its bars start with the intervals");
  CHECK_EQ(syncTooltip("Bitwig Studio", LinkState::Waiting, 0),
           "Sync with Bitwig Studio: when you change its tempo, vote the room to it; and while it plays, start the intervals on "
           "its bars (a multiple of BPI beats from bar 1). In a room, with Link on in Bitwig Studio (if it has Link), it takes the "
           "room's tempo instead, and its bars start with the intervals");
  // no Link app: say to turn it on in Live
  CHECK_EQ(syncTooltip(live, LinkState::Leading, 0),
           "Turn on Link in Live: then Live takes the room's tempo, and its bars start with the intervals. Until then, Sync votes "
           "the room to Live's tempo when you change it, and starts the intervals on its bars while it plays");
  CHECK_EQ(syncTooltip(live, LinkState::OtherInstance, 1), plain + ". Another Jamsa in this set leads Link");
  CHECK_EQ(syncTooltip(live, LinkState::Contested, 1),
           plain + ". Another app on the network keeps leading Link, so this one stopped: turn Sync off and on to lead it again");
  CHECK_EQ(syncTooltip(live, LinkState::Unavailable, 0), plain + ". Link couldn't start here");
  CHECK_EQ(syncTooltip("REAPER", LinkState::Leading, 1).compare(0, 31, "Sync with REAPER through Link: "), 0);
}

// Link for real (Link's own threads, in this program): one instance leads it, and the room's
// tempo and intervals go to it in real time; a second instance can't lead it at the same time,
// and leads it once the first stops. Link is on only while an instance uses it.
static void test_link_sync()
{
  CHECK(!LinkSync::enabledInProgram());
  {
    LinkSync a, b;
    CHECK(!a.drive()); // Sync is off: not in Link
    CHECK(!LinkSync::enabledInProgram() && a.numPeers() == 0 && a.sessionTempo() == 0);
    a.use(true);
    b.use(true);
    CHECK(LinkSync::enabledInProgram() && a.inUse() && b.inUse());
    CHECK(LinkSync::enabledFor() >= 0 && LinkSync::enabledFor() < 5);
    CHECK(a.drive() && a.driving());
    CHECK(a.drive()); // again: still
    CHECK(!b.drive() && !b.driving());
    CHECK(LinkSync::otherInstanceDrives(&b) && !LinkSync::otherInstanceDrives(&a));

    const int srate = 48000, frames = 480;
    LinkSync::Block block;
    block.frames = frames;
    block.sampleRate = srate;
    LinkSync::Interval iv;
    iv.running = true;
    iv.bpi = 4;
    iv.bpm = 100;
    iv.len = 60 * srate * iv.bpi / 100;
    iv.pos = 1234;
    LinkSync::Host host; // a host that doesn't say where it is
    int leads = 0, blocks = 0;
    // in real time, with calls late now and then (a timer that wakes up late, a busy machine),
    // and the next ones back to back until it catches up
    auto run = [&](LinkSync &l, double seconds) {
      const auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < seconds * srate / frames; i ++)
      {
        leads += l.process(block, iv, host) ? 1 : 0;
        blocks++;
        block.sampleTime += frames;
        iv.pos += frames;
        if (iv.pos >= iv.len) iv.pos -= iv.len;
        std::this_thread::sleep_until(start + std::chrono::microseconds((long long)((i + 1) * 1e6 * frames / srate)));
        if (i % 7 == 3) std::this_thread::sleep_for(std::chrono::milliseconds(i % 21 == 3 ? 40 : 12));
      }
    };
    run(a, 1.0);
    CHECK(near(a.sessionTempo(), 100, 1e-6));
    CHECK(a.jumps() >= 1);
    CHECK(a.aligned());
    CHECK(fabs(a.lastErrorMs()) < 2.0);
    // Link leads the host's bars only with another Link app (Live with its Link on) there
    CHECK(a.numPeers() > 0 ? leads > 0 : leads == 0);
    printf("Link: tempo %.3f, error %.3f ms, %d jumps, %d steps, %d peers\n", a.sessionTempo(), a.lastErrorMs(), a.jumps(), a.steps(),
           (int)a.numPeers());

    // the other instance's process() leaves Link alone
    CHECK(!b.process(block, iv, host));
    CHECK_EQ(b.jumps(), 0);

    // the room's tempo and BPI change at an interval: Link follows, and the beat lines up again
    const int jumps = a.jumps();
    iv.bpm = 120;
    iv.bpi = 8;
    iv.len = 60 * srate * iv.bpi / 120;
    iv.pos = 0;
    run(a, 1.0);
    CHECK(near(a.sessionTempo(), 120, 1e-6));
    CHECK_EQ(a.jumps(), jumps + 1);
    CHECK(a.aligned());

    // the room's tempo back on Link (after something else changed it)
    a.restoreTempo();
    CHECK(a.restorePending());
    run(a, 0.2);
    CHECK(!a.restorePending());
    CHECK(near(a.sessionTempo(), 120, 1e-6));

    // no intervals (not in a room yet, or no audio): Link is left alone
    iv.running = false;
    CHECK(!a.process(block, iv, host));
    iv.running = true;

    // the first stops leading; the second leads from where Link is
    a.release();
    CHECK(!a.driving() && !LinkSync::otherInstanceDrives(&b));
    CHECK(!a.process(block, iv, host));
    CHECK(b.drive());
    iv.bpm = 90;
    iv.len = 60 * srate * iv.bpi / 90;
    iv.pos = 0;
    run(b, 0.5);
    CHECK(near(b.sessionTempo(), 90, 1e-6));
    CHECK(near(a.sessionTempo(), 90, 1e-6)); // one Link in the program
    CHECK(b.jumps() >= 1 && b.aligned());

    // Sync off in both: not in Link at all
    a.use(false);
    CHECK(LinkSync::enabledInProgram());
    b.use(false); // also stops leading it
    CHECK(!b.driving() && !LinkSync::enabledInProgram() && b.sessionTempo() == 0);
    CHECK(!LinkSync::otherInstanceDrives(nullptr));
    // and again
    a.use(true);
    CHECK(LinkSync::enabledInProgram() && a.drive());
  }
  CHECK(!LinkSync::enabledInProgram()); // the last instance gone
}

static void test_names()
{
  CHECK_EQ(Engine::displayName("alice@1.2.3.x"), "alice");
  CHECK_EQ(Engine::displayName("bob"), "bob");
  // the second of two players with one name ("AnonymousUsers multi")
  CHECK_EQ(Engine::displayName("guest@127.0.0.1.2"), "guest.2");
  CHECK_EQ(Engine::displayName("guest@1.2.3.x.16"), "guest.16");
  CHECK_EQ(Engine::displayName("guest@127.0.0.1"), "guest");
  CHECK_EQ(Engine::displayName("guest@::ffff:127.0.0.1.3"), "guest.3");
  CHECK_EQ(Engine::displayName("guest@::ffff:127.0.0.1"), "guest");
  CHECK_EQ(Engine::remoteKey("Guest@127.0.0.1.2", 0), "guest.2/0");
  CHECK_EQ(Engine::remoteKey("Alice@1.2.3.x", 2), "alice/2");

  // the name a new instance starts with, from the computer account
  CHECK_EQ(Engine::firstName("Leo Stern"), "Leo");
  CHECK_EQ(Engine::firstName("  J\xc3\xb6rg  M\xc3\xbcller "), "J\xc3\xb6rg");
  CHECK_EQ(Engine::firstName("leo"), "leo");
  CHECK_EQ(Engine::firstName("leo@example.com"), "leo");
  CHECK_EQ(Engine::firstName(""), "");
  CHECK_EQ(Engine::firstName(std::string(40, 'a')), std::string(32, 'a'));
  CHECK(!Engine::defaultUserName().empty()); // the test runs as some account
  {
    Engine en;
    const std::string def = en.getSettings().userName;
    CHECK_EQ(def, Engine::defaultUserName());
    Settings saved; // a set saved before a name was typed keeps the default
    en.setSettings(saved);
    CHECK_EQ(en.getSettings().userName, def);
    saved.userName = "typed";
    en.setSettings(saved);
    CHECK_EQ(en.getSettings().userName, "typed");
  }
}

// The click, rendered as the engine does: in blocks of any size that stop at the end of each
// interval. `lengths` are how long the intervals are (one may be shorter or longer than usual,
// as when it lines up with the host's bars); `onsets` gets where each beat should start.
static std::vector<float> render_clicks(double sr, const std::vector<int> &lengths, int usual, int bpi, float volume,
                                        std::vector<long> &onsets, std::vector<bool> &accents)
{
  long total = 0;
  for (int l : lengths) total += l;
  std::vector<float> left(total, 0.0f), right(total, 0.0f);
  Click click;
  click.setSampleRate(sr);
  unsigned seed = 12345;
  long t = 0;
  for (size_t iv = 0; iv < lengths.size(); iv ++)
  {
    for (int k = 0; k < bpi; k ++)
    {
      const long p = (long)llround((double)k * usual / bpi);
      if (p < lengths[iv])
      {
        onsets.push_back(t + p);
        accents.push_back(k == 0);
      }
    }
    for (int pos = 0; pos < lengths[iv]; )
    {
      seed = seed * 1103515245u + 12345u;
      const int n = std::min(1 + (int)((seed >> 16) % 700), lengths[iv] - pos);
      click.render(&left[t], &right[t], n, pos, usual, bpi, volume);
      pos += n;
      t += n;
    }
  }
  CHECK(left == right);
  return left;
}

// how much of x[from, to) is at hz (Goertzel), per sample
static double tone_power(const std::vector<float> &x, long from, long to, double hz, double sr)
{
  const double w = 2 * 3.14159265358979 * hz / sr, c = 2 * cos(w);
  double s1 = 0, s2 = 0;
  for (long i = from; i < to; i ++)
  {
    const double s0 = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return (s1 * s1 + s2 * s2 - c * s1 * s2) / (double)(to - from);
}

static void test_click()
{
  // the click itself
  CHECK_EQ(Click::sample(true, 0, 48000), 0.0);
  CHECK_EQ(Click::sample(false, 0, 48000), 0.0);
  CHECK_EQ(Click::sample(true, Click::length(48000), 48000), 0.0);
  CHECK_EQ(Click::length(48000), 1920);
  CHECK_EQ(Click::length(44100), 1764);
  CHECK_EQ(Click::length(96000), 3840);

  const float volume = 0.5f;
  struct Heard { double peakMs, quietMs, energy, accentPeak, beatPeak; };
  std::vector<Heard> heard;
  for (double sr : { 44100.0, 48000.0, 96000.0 })
  {
    // 120 BPM, 4 beats: three intervals of 2 seconds, but the second ends 0.7 seconds
    // early (it lines up with the host's bars), and the third 0.3 seconds late
    const int usual = (int)(2.0 * sr), bpi = 4;
    const std::vector<int> lengths = { usual, usual - (int)(0.7 * sr), usual + (int)(0.3 * sr) };
    std::vector<long> onsets;
    std::vector<bool> accents;
    const std::vector<float> x = render_clicks(sr, lengths, usual, bpi, volume, onsets, accents);
    CHECK_EQ(onsets.size(), (size_t)11); // 4 + 3 (the fourth beat of the short one never comes) + 4
    const int len = Click::length(sr);
    const double slope = 2 * 3.14159265358979 * Click::ACCENT_HZ / sr; // how fast a full-level sine at 1500 Hz can change

    // each click starts on its beat, from silence, and ends in silence: nothing sounds anywhere else
    std::vector<bool> inClick(x.size(), false);
    Heard h = { 0, 0, 0, 0, 1 };
    for (size_t b = 0; b < onsets.size(); b ++)
    {
      const long p = onsets[b];
      for (long i = p; i < std::min<long>(p + len, (long)x.size()); i ++) inClick[i] = true;
      CHECK(p == 0 || x[p - 1] == 0.0f); // silence before
      CHECK_EQ(x[p], 0.0f);              // the attack starts at 0 on the beat's sample
      CHECK(x[p + 1] != 0.0f);           // and sounds from the next one
      CHECK(fabsf(x[p + 1]) < 0.001f * volume && fabsf(x[p + 2]) < 0.004f * volume); // no step at the start
      CHECK(fabsf(x[p + len - 1]) < 1e-4f * volume);                                  // nor at the end
      CHECK(p + len >= (long)x.size() || x[p + len] == 0.0f);
      double worstStep = 0, peak = 0;
      long peakAt = p;
      for (long i = p; i < p + len; i ++)
      {
        worstStep = std::max(worstStep, (double)fabsf(x[i + 1] - x[i]));
        if (fabsf(x[i]) > peak) { peak = fabsf(x[i]); peakAt = i; }
      }
      CHECK(worstStep <= volume * slope * 1.02); // no step anywhere: only the sine's own slope

      // beat 1 of each interval is the accent: 1500 Hz and louder; the others 1000 Hz at 60%
      const double hi = tone_power(x, p, p + len, Click::ACCENT_HZ, sr), lo = tone_power(x, p, p + len, Click::BEAT_HZ, sr);
      if (accents[b])
      {
        CHECK(hi > lo * 20);
        CHECK(peak > 0.9 * volume && peak <= volume);
        h.accentPeak = peak;
        h.peakMs = (peakAt - p) * 1000.0 / sr;
        long quiet = p;
        for (long i = p; i < p + len; i ++)
          if (fabsf(x[i]) > peak * 0.001) quiet = i;
        h.quietMs = (quiet - p) * 1000.0 / sr;
        h.energy = 0;
        for (long i = p; i < p + len; i ++) h.energy += x[i] * (double)x[i] / sr;
      }
      else
      {
        CHECK(lo > hi * 20);
        CHECK(peak > 0.54 * volume && peak <= 0.6 * volume);
        h.beatPeak = std::min(h.beatPeak, peak);
      }
    }
    for (size_t i = 0; i < x.size(); i ++)
      if (!inClick[i] && x[i] != 0.0f)
      {
        CHECK(!"a sound outside the clicks");
        break;
      }
    printf("click at %.0f Hz: accent peak %.3f at %.2f ms, -60 dB at %.1f ms, energy %.3g; other beats peak %.3f\n", sr, h.accentPeak,
           h.peakMs, h.quietMs, h.energy, h.beatPeak);
    heard.push_back(h);

    // muted (volume 0): nothing
    std::vector<long> o2;
    std::vector<bool> a2;
    const std::vector<float> silent = render_clicks(sr, { usual }, usual, bpi, 0.0f, o2, a2);
    CHECK(std::all_of(silent.begin(), silent.end(), [](float v) { return v == 0.0f; }));
  }

  // it sounds the same at 44.1, 48 and 96 kHz
  for (const Heard &h : heard)
  {
    CHECK(fabs(h.peakMs - heard[1].peakMs) < 0.35);   // within half a period of 1500 Hz
    CHECK(fabs(h.quietMs - heard[1].quietMs) < 0.35);
    CHECK(fabs(h.energy / heard[1].energy - 1) < 0.01);
    CHECK(fabs(h.accentPeak - heard[1].accentPeak) < 0.01);
  }
  CHECK(heard[1].quietMs > 30 && heard[1].quietMs < 40); // -60 dB in 30 to 40 ms
}

static void test_uitext()
{
  using namespace uitext;

  // dragging the BPM box: 2 pixels a BPM, 8 with Shift; whole steps only, toward the start
  CHECK_EQ(dragBpm(120, 0, false), 120);
  CHECK_EQ(dragBpm(120, 1.9, false), 120);
  CHECK_EQ(dragBpm(120, 2, false), 121);
  CHECK_EQ(dragBpm(120, -2, false), 119);
  CHECK_EQ(dragBpm(120, -3.9, false), 119);
  CHECK_EQ(dragBpm(120, 20, false), 130);
  CHECK_EQ(dragBpm(120, 20, true), 122);
  CHECK_EQ(dragBpm(120, -7.9, true), 120);
  CHECK_EQ(dragBpm(120, 1000, false), BPM_MAX);
  CHECK_EQ(dragBpm(120, -1000, false), BPM_MIN);
  CHECK_EQ(dragBpm(300, 0, false), BPM_MAX); // a room faster than the box goes: the box's range

  // typing in it
  int bpm = 0;
  CHECK(parseBpm("125", &bpm) && bpm == 125);
  CHECK(parseBpm(" 125.4 ", &bpm) && bpm == 125);
  CHECK(parseBpm("125.5", &bpm) && bpm == 126);
  CHECK(parseBpm("95,5", &bpm) && bpm == 96);
  CHECK(parseBpm("100 bpm", &bpm) && bpm == 100);
  CHECK(parseBpm("100BPM", &bpm) && bpm == 100);
  CHECK(parseBpm(".9", &bpm) && bpm == BPM_MIN);
  CHECK(parseBpm("20", &bpm) && bpm == BPM_MIN);
  CHECK(parseBpm("999", &bpm) && bpm == BPM_MAX);
  CHECK(parseBpm("99999999999999", &bpm) && bpm == BPM_MAX);
  bpm = 77;
  CHECK(!parseBpm("", &bpm));
  CHECK(!parseBpm("   ", &bpm));
  CHECK(!parseBpm("fast", &bpm));
  CHECK(!parseBpm("120 beats", &bpm));
  CHECK(!parseBpm("-120", &bpm));
  CHECK(!parseBpm(".", &bpm));
  CHECK(!parseBpm("1e3", &bpm));
  CHECK_EQ(bpm, 77);

  CHECK_EQ(bpmBoxText(120), std::string("120.00"));
  CHECK_EQ(bpmBoxText(95.456), std::string("95.46"));
  CHECK_EQ(bpmBoxText(239.999), std::string("240.00"));
  CHECK_EQ(bpmBoxText(0), std::string("\xe2\x80\x94"));
  CHECK_EQ(bpmBoxText(-5), std::string("\xe2\x80\x94"));

  CHECK_EQ(dbText(1.0f), std::string("0.0 dB"));
  CHECK_EQ(dbText(0.9999f), std::string("0.0 dB"));
  CHECK_EQ(dbText(2.0f), std::string("+6.0 dB"));
  CHECK_EQ(dbText(0.25f), std::string("-12.0 dB"));
  CHECK_EQ(dbText(0.5f), std::string("-6.0 dB"));
  CHECK_EQ(dbText(0.989f), std::string("-0.1 dB"));
  CHECK_EQ(dbText(0.0f), std::string("-inf dB"));
  CHECK_EQ(dbText(-1.0f), std::string("-inf dB"));

  // the meter: nothing for silence, all seven at 0 dB, the yellow two only near the top
  CHECK_EQ(litSegments(0.0f), 0);
  CHECK_EQ(litSegments(0.001f), 0);          // -60 dB
  CHECK_EQ(litSegments(0.004f), 1);          // -48 dB
  CHECK_EQ(litSegments(0.1f), 3);            // -20 dB
  CHECK_EQ(litSegments(0.5f), 5);            // -6 dB: all the green ones
  CHECK_EQ(litSegments(0.6f), 6);            // -4.4 dB
  CHECK_EQ(litSegments(0.95f), 7);           // -0.4 dB
  CHECK_EQ(litSegments(1.0f), METER_SEGMENTS);
  CHECK_EQ(litSegments(4.0f), METER_SEGMENTS);
  for (int i = 1; i < 1000; i++) CHECK(litSegments(i / 1000.0f) >= litSegments((i - 1) / 1000.0f));

  CHECK_EQ(beatText(0, 48000, 16), std::string("1 / 16"));
  CHECK_EQ(beatText(47999, 48000, 16), std::string("16 / 16"));
  CHECK_EQ(beatText(48000, 48000, 16), std::string("16 / 16"));
  CHECK_EQ(beatText(6000, 48000, 16), std::string("3 / 16"));
  CHECK_EQ(beatText(5999, 48000, 16), std::string("2 / 16"));
  CHECK_EQ(beatText(0, 0, 16), std::string("\xe2\x80\x93 / 16"));
  CHECK_EQ(beatText(0, 48000, 0), std::string("\xe2\x80\x93 / \xe2\x80\x93"));

  CHECK_EQ(kbpsText(128), std::string("128k"));

  // the BPI list holds the room's usual choices, in order
  CHECK_EQ((int)(sizeof(BPI_CHOICES) / sizeof(BPI_CHOICES[0])), NUM_BPI_CHOICES);
  for (int i = 1; i < NUM_BPI_CHOICES; i++) CHECK(BPI_CHOICES[i] > BPI_CHOICES[i - 1]);
  CHECK_EQ(BPI_CHOICES[0], 2); // the server's least
  CHECK_EQ(BPI_CHOICES[NUM_BPI_CHOICES - 1], 64); // and its most
}

static void test_textkeys()
{
  using namespace textkeys;

  // Cmd (macOS) or Ctrl (elsewhere) with X, C, V, A; nothing else held
  CHECK(commandFor('x', true, false, false, false) == Command::Cut);
  CHECK(commandFor('c', true, false, false, false) == Command::Copy);
  CHECK(commandFor('v', true, false, false, false) == Command::Paste);
  CHECK(commandFor('a', true, false, false, false) == Command::SelectAll);
  CHECK(commandFor('c', false, false, false, false) == Command::None); // typing a c
  CHECK(commandFor('z', true, false, false, false) == Command::None);
  CHECK(commandFor(0, true, false, false, false) == Command::None);
  CHECK(commandFor('v', true, true, false, false) == Command::None);  // Shift: another shortcut
  CHECK(commandFor('c', true, false, true, false) == Command::None);  // AltGr+C types a letter (Polish c-acute)
  CHECK(commandFor('a', true, false, false, true) == Command::None);  // Cmd+Ctrl, Ctrl+Win
  CHECK(commandFor('x', false, false, false, true) == Command::None); // Ctrl alone on macOS

  // the letter of what a key types: either case, or the control character Ctrl types
  CHECK(letterOf('c') == 'c' && letterOf('C') == 'c' && letterOf('v') == 'v' && letterOf('X') == 'x');
  CHECK(letterOf(3) == 'c' && letterOf(22) == 'v' && letterOf(24) == 'x' && letterOf(1) == 'a');
  CHECK(letterOf(0) == 0 && letterOf('1') == 0 && letterOf(' ') == 0 && letterOf(0x441) == 0 && letterOf(27) == 0);
  CHECK(commandFor(letterOf(22), true, false, false, false) == Command::Paste); // a host that hands on Ctrl+V as 0x16

  // the Latin letter of a key on macOS: a US or French layout types it; Russian and Greek
  // type their own without Cmd (the second) and Latin with it (the first), or else its place
  CHECK(latinLetter({ 'c', 'c' }, 'c') == 'c');
  CHECK(latinLetter({ 'q', 'q' }, 'a') == 'q');      // AZERTY: the key where US has A is Q
  CHECK(latinLetter({ 'a', 'a' }, 0) == 'a');        // and A is where US has Q
  CHECK(latinLetter({ 'c', 0x441 }, 'c') == 'c');    // Russian with Cmd: c, then es
  CHECK(latinLetter({ 0x441, 0x441 }, 'c') == 'c');  // a layout without Latin letters: the key's place
  CHECK(latinLetter({ 0x3c8, 0x3c8 }, 'v') == 'v');  // Greek psi on the V key
  CHECK(latinLetter({ 0, 0 }, 0) == 0);
  CHECK(latinLetter({ 'V' }, 0) == 'v');
  CHECK(macUsLetter(0x00) == 'a' && macUsLetter(0x07) == 'x' && macUsLetter(0x08) == 'c' && macUsLetter(0x09) == 'v');
  CHECK(macUsLetter(0x01) == 0 && macUsLetter(0x24) == 0); // S, Return

  // pasted into a box of one line
  CHECK_EQ(oneLine("203.0.113.7:2049"), std::string("203.0.113.7:2049"));
  CHECK_EQ(oneLine("203.0.113.7:2049\n"), std::string("203.0.113.7:2049"));     // copied with its line break
  CHECK_EQ(oneLine("\r\n  ninbot.com:2049  \r\n"), std::string("ninbot.com:2049"));
  CHECK_EQ(oneLine("first line\nsecond\r\n\r\nthird\rfourth"), std::string("first line second third fourth"));
  CHECK_EQ(oneLine("  two  spaces  "), std::string("  two  spaces  "));        // one line keeps its spaces
  CHECK_EQ(oneLine("a\tb"), std::string("a b"));
  CHECK_EQ(oneLine("bell\x07 and \x7f"), std::string("bell and "));
  CHECK_EQ(oneLine("\n\n"), std::string(""));
  CHECK_EQ(oneLine(""), std::string(""));
  CHECK_EQ(oneLine("J\xc3\xb6rg\n\xe2\x80\x94 hi"), std::string("J\xc3\xb6rg \xe2\x80\x94 hi")); // UTF-8 stays
}

#if defined(__linux__)
// another program copies something: it takes the CLIPBOARD selection (and answers nobody)
static bool take_clipboard_elsewhere(xcb_connection_t **keep)
{
  xcb_connection_t *c = xcb_connect(nullptr, nullptr);
  if (!c || xcb_connection_has_error(c)) return false;
  const xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
  const xcb_window_t w = xcb_generate_id(c);
  xcb_create_window(c, XCB_COPY_FROM_PARENT, w, screen->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, nullptr);
  xcb_intern_atom_reply_t *a = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, 9, "CLIPBOARD"), nullptr);
  if (!a) return false;
  xcb_set_selection_owner(c, w, a->atom, XCB_CURRENT_TIME);
  xcb_get_selection_owner_reply_t *r = xcb_get_selection_owner_reply(c, xcb_get_selection_owner(c, a->atom), nullptr);
  const bool ok = r && r->owner == w;
  free(r);
  free(a);
  *keep = c;
  return ok;
}

static void test_clipboard()
{
  if (!getenv("DISPLAY") || !*getenv("DISPLAY"))
  {
    printf("no X server (DISPLAY); skipped the clipboard checks\n");
    return;
  }
  std::string got;
  const std::string lan = "192.168.1.20:2049";
  CHECK(clipboard::setText(lan));
  CHECK(clipboard::readX11(&got, 2.0) && got == lan);
  CHECK(clipboard::readX11(&got, 2.0) && got == lan); // pasted again
  got.clear();
  CHECK(clipboard::getText(&got) && got == lan);       // what a paste in a text box reads

  // copied again: the new text
  const std::string wan = "J\xc3\xb6rg's room \xc2\xb7 203.0.113.7:2049";
  CHECK(clipboard::setText(wan));
  CHECK(clipboard::readX11(&got, 2.0) && got == wan);

  // another program copies something: that's what is pasted now, and copying again takes it back
  xcb_connection_t *other = nullptr;
  CHECK(take_clipboard_elsewhere(&other));
  got = "?";
  CHECK(!clipboard::readX11(&got, 0.5)); // the other program doesn't answer
  CHECK(!clipboard::getText(&got) && got == "?");
  CHECK(clipboard::setText(lan));
  CHECK(clipboard::readX11(&got, 2.0) && got == lan);
  if (other) xcb_disconnect(other);

  // a long text, and one too long to hand over in one piece
  const std::string big(50000, 'x');
  CHECK(clipboard::setText(big));
  CHECK(clipboard::readX11(&got, 2.0) && got == big);
  CHECK(!clipboard::setText(std::string(70000, 'y')));
  CHECK(clipboard::readX11(&got, 2.0) && got == big); // the last one stays

  // the module is unloaded: it lets go; a copy after that works again
  clipboard::stopX11();
  CHECK(!clipboard::readX11(&got, 0.5));
  CHECK(!clipboard::getText(&got)); // nobody has the clipboard: nothing to paste
  CHECK(clipboard::setText(wan));
  CHECK(clipboard::readX11(&got, 2.0) && got == wan);
  clipboard::stopX11();
  printf("clipboard: copied and pasted\n");
}
#endif

int main(int argc, char **argv)
{
  if (argc > 1 && !strcmp(argv[1], "clipboard"))
  {
#if defined(__linux__)
    test_clipboard();
#else
    printf("the clipboard checks are for Linux\n");
#endif
    printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
  }

  test_state();
  test_live_commands();
  test_hostserver_live();
  test_portmap_messages();
  test_names();
  test_bar_math();
  test_bar_sync();
  test_tempo_follower();
  test_host_note();
  test_click();
  test_uitext();
  test_textkeys();
  test_link_math();
  test_block_clock();
  test_calibrator();
  test_tempo_keeper();
  test_link_texts();
  test_link_sync();

  // the same in a locale that writes 95,5
  const char *loc = nullptr;
  for (const char *name : { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "German_Germany.1252" })
    if ((loc = setlocale(LC_ALL, name))) break;
  if (loc)
  {
    printf("again with the locale %s\n", loc);
    test_state();
    test_uitext();
    setlocale(LC_ALL, "C");
  }
  else printf("no comma locale here; skipped the locale checks\n");

  printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
