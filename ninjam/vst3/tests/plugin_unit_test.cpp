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
  state, reading the built-in server's port mapping messages, and following
  the host: lining the intervals up with its bars, voting for its tempo, and
  the editor's note on its tempo.

*/

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "engine.h"
#include "hostserver.h"
#include "hostsync.h"

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
  CHECK_EQ(e.opusKbps, 128);

  // the Opus bitrate is one of the choices: the nearest one, or the default for junk
  struct { const char *saved; int kbps; } bitrates[] = {
    { "64", 64 }, { "96", 96 }, { "160", 160 }, { "256", 256 }, { "510", 510 }, { "300", 256 }, { "999999999999", 510 },
    { "-5", 64 }, { "abc", 128 }, { "", 128 }, { "128x", 128 },
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
  CHECK_EQ(old.opusKbps, 128);

  // Follow Ableton's tempo: on by default, and in states saved before it was a setting
  CHECK(Settings().followHostTempo);
  CHECK(old.followHostTempo);
  Settings off;
  off.followHostTempo = false;
  Settings offLoaded;
  CHECK(offLoaded.deserialize(off.serialize()));
  CHECK(!offLoaded.followHostTempo);
  CHECK(e.followHostTempo);
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
  void play(int frames)
  {
    if (!p.playing) return;
    p.ppq += frames / srate * p.tempo / 60;
    if (p.cycleActive && p.ppq >= p.cycleEnd) p.ppq -= p.cycleEnd - p.cycleStart;
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
      const int end = sync.process(host.p, frames, host.srate, i);
      if (end > 0) iv.setEnd(end);
      iv.play(frames);
      host.play(frames);
    }
  }
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
  bool regular(long from) const
  {
    for (size_t k = 1; k < iv.starts.size(); k ++)
      if (iv.starts[k - 1] >= from && iv.starts[k] - iv.starts[k - 1] != iv.len) return false;
    return true;
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
  CHECK_EQ(shortHostName("Ableton Live 12 Suite"), "Ableton");
  CHECK_EQ(shortHostName(" Live "), "Ableton");
  CHECK_EQ(shortHostName("Bitwig Studio"), "Bitwig Studio");
  CHECK_EQ(shortHostName(""), "The DAW");
  CHECK_EQ(shortHostName("A very long host name that goes on and on"), "A very long host name that goes ");
  CHECK_EQ(shortHostName("\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9x\xc3\xa9\xc3\xa9"),
           "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9x");

  CHECK_EQ(bpmText(120), "120");
  CHECK_EQ(bpmText(109.6), "109.6");
  CHECK_EQ(bpmText(120.25), "120.25");
  CHECK_EQ(hostTempoNote("Ableton Live 12 Suite", 120, 95, true), "Ableton is at 120 BPM, the room at 95: set Ableton to 95");
  CHECK_EQ(hostTempoNote("Ableton Live 12 Suite", 120, 95, false),
           "Ableton is at 120 BPM, the room at 95: set Ableton to 95, or turn on Follow Ableton's tempo");
  CHECK_EQ(hostTempoNote("Bitwig Studio", 95.3, 95, true), "Bitwig Studio is at 95.3 BPM, the room at 95: set its tempo to 95");
  CHECK_EQ(hostTempoNote("Ableton Live 12", 95.004, 95, true), "");
  CHECK_EQ(hostTempoNote("Ableton Live 12", 0, 95, true), "");
  CHECK_EQ(hostTempoNote("Ableton Live 12", 120, 0, true), "");
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
}

int main()
{
  test_state();
  test_portmap_messages();
  test_names();
  test_bar_math();
  test_bar_sync();
  test_tempo_follower();
  test_host_note();

  // the same in a locale that writes 95,5
  const char *loc = nullptr;
  for (const char *name : { "de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "German_Germany.1252" })
    if ((loc = setlocale(LC_ALL, name))) break;
  if (loc)
  {
    printf("again with the locale %s\n", loc);
    test_state();
    setlocale(LC_ALL, "C");
  }
  else printf("no comma locale here; skipped the locale checks\n");

  printf("%d checks, %d failed\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
