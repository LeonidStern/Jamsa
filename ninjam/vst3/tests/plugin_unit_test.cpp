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
  state, and reading the built-in server's port mapping messages.

*/

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "engine.h"
#include "hostserver.h"

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

static void test_names()
{
  CHECK_EQ(Engine::displayName("alice@1.2.3.x"), "alice");
  CHECK_EQ(Engine::displayName("bob"), "bob");
  CHECK_EQ(Engine::remoteKey("Alice@1.2.3.x", 2), "alice/2");
}

int main()
{
  test_state();
  test_portmap_messages();
  test_names();

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
