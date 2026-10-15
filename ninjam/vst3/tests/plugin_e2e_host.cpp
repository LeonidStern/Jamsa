/*
    NINJAM VST3 - plugin_e2e_host.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  A small VST3 host for the end-to-end test (test_plugin_e2e.py).

  It loads NINJAM.vst3, plays a 997 Hz tone into it in real time, and drives
  it through its text command interface.

  Usage: plugin_e2e_host path/to/NINJAM.vst3 [samplerate [blocksize [join host:port | two]]]

  Hosting (the default): the plug-in hosts a server and joins it, and a second
  NJClient ("peer", in this process) joins too and plays a 661 Hz tone. The
  room goes from Opus to FLAC and back by typing !codec in the plug-in's chat,
  then the plug-in's Opus bitrate goes from 128 to 256 and 510 kbps, and last
  the tempo goes to 60 BPM and 16 beats (16 second intervals) at 510 kbps. For
  each phase the host measures how well each side hears the other's tone (SNR).
    PORT <port>            the plug-in's server is up; connect the watcher, then write "go"
    PHASE <name> BEGIN     from here to END, the plug-in uploads as the phase says
    PHASE <name> END room=<codec> upload=<fourcc> kbps=<n> plugin_hears_snr=<dB> peer_hears_snr=<dB>
    DONE

  Joining (join host:port): the plug-in joins a server whose room plays Vorbis
  (older clients are there), so it can't send, and hears Vorbis as silence.
    JOINED                 connected; write "go" once the Vorbis clients play
    PHASE vorbis BEGIN     the room plays Vorbis
    PHASE vorbis END room=<codec> upload=<fourcc or -> peak=<the plug-in's output peak> warned=<0|1> others=<n>
    DONE

  Two instances (two): as two tracks in a DAW, two instances of the plug-in in this
  process, the second with its audio on a thread of its own. "alice" hosts a server;
  "bob" can't host a second one in this process and is told to join, joins, and they
  hear each other (alice plays 997 Hz, bob 661 Hz). Then alice stops her server, which
  drops bob; bob hosts one, and alice joins it.
    PORT <port>            alice's server is up
    PHASE two BEGIN
    PHASE two END a_hears_snr=<dB> b_hears_snr=<dB> b_host_refused=<0|1> b_got_chat=<0|1> a_users=<n> b_users=<n>
    PHASE swap END b_dropped=<0|1> b_hosting=<0|1> port=<port> a_state=<state> a_users=<n>
    DONE

  Errors start with "error:" and the exit code is not 0. With NINJAM_E2E_DEBUG set, each
  phase also prints the level of what each side heard, second by second.

*/

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"

#include "njclient.h"
#include "pluginids.h"

namespace ninjam_vst3 { DEF_CLASS_IID(INinjamCommand) }

using namespace Steinberg;
using namespace Steinberg::Vst;

static const double PI = 3.14159265358979;
static const double PLUGIN_FREQ = 997.0, PEER_FREQ = 661.0;

static double now_seconds()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static double tone_snr(const std::vector<float> &x, size_t start, size_t end, double freq, int srate)
{
  double ss = 0, sc = 0, cc = 0, sx = 0, cx = 0;
  for (size_t i = start; i < end; i ++)
  {
    const double s = sin(2*PI*freq*i/srate), c = cos(2*PI*freq*i/srate);
    ss += s*s; cc += c*c; sc += s*c; sx += s*x[i]; cx += c*x[i];
  }
  const double det = ss*cc - sc*sc;
  if (det <= 0) return -100.0;
  const double a = (sx*cc - cx*sc) / det, b = (cx*ss - sx*sc) / det;
  double sig = 0, err = 0;
  for (size_t i = start; i < end; i ++)
  {
    const double fit = a*sin(2*PI*freq*i/srate) + b*cos(2*PI*freq*i/srate);
    sig += fit*fit;
    err += (x[i]-fit)*(x[i]-fit);
  }
  return sig > 0 ? 10*log10(sig / (err + 1e-20)) : -100.0;
}

static std::map<std::string, std::string> parse_status(const std::string &s)
{
  std::map<std::string, std::string> m;
  size_t i = 0;
  while (i < s.size())
  {
    size_t sp = s.find(' ', i);
    if (sp == std::string::npos) sp = s.size();
    const std::string tok = s.substr(i, sp - i);
    const size_t eq = tok.find('=');
    if (eq != std::string::npos) m[tok.substr(0, eq)] = tok.substr(eq + 1);
    i = sp + 1;
  }
  return m;
}

static bool stdin_has_line(std::string &line)
{
  fd_set fds;
  FD_ZERO(&fds);
  FD_SET(0, &fds);
  timeval tv = { 0, 0 };
  if (select(1, &fds, nullptr, nullptr, &tv) <= 0) return false;
  char buf[256];
  if (!fgets(buf, sizeof(buf), stdin)) return false;
  line = buf;
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
  return true;
}

class Host
{
public:
  Host(int srate, int block, double freq = PLUGIN_FREQ) : m_srate(srate), m_block(block), m_freq(freq) {}

  bool load(const char *path)
  {
    std::string err;
    m_module = VST3::Hosting::Module::create(path, err);
    if (!m_module)
    {
      printf("error: could not load %s: %s\n", path, err.c_str());
      return false;
    }
    m_hostApp = owned(new HostApplication);
    m_module->getFactory().setHostContext(m_hostApp);
    return createInstance();
  }

  // another instance from the module that first.load() loaded, like a second track
  bool loadAnother(const Host &first)
  {
    m_module = first.m_module;
    m_hostApp = first.m_hostApp;
    return createInstance();
  }

  bool createInstance()
  {
    const auto &factory = m_module->getFactory();
    for (auto &ci : factory.classInfos())
    {
      if (ci.category() != kVstAudioEffectClass || ci.name() != "NINJAM") continue;
      m_component = factory.createInstance<IComponent>(ci.ID());
      break;
    }
    if (!m_component)
    {
      printf("error: no NINJAM audio effect class in the module\n");
      return false;
    }
    if (m_component->initialize(m_hostApp) != kResultOk)
    {
      printf("error: initialize failed\n");
      return false;
    }
    m_processor = U::cast<IAudioProcessor>(m_component);
    m_commands = U::cast<ninjam_vst3::INinjamCommand>(m_component);
    if (!m_processor || !m_commands)
    {
      printf("error: the component has no IAudioProcessor or INinjamCommand\n");
      return false;
    }

    SpeakerArrangement in = SpeakerArr::kStereo, out = SpeakerArr::kStereo;
    if (m_processor->setBusArrangements(&in, 1, &out, 1) != kResultTrue)
    {
      printf("error: the plug-in refused a stereo in, stereo out arrangement\n");
      return false;
    }
    ProcessSetup setup {};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = m_block;
    setup.sampleRate = m_srate;
    if (m_processor->setupProcessing(setup) != kResultOk)
    {
      printf("error: setupProcessing failed\n");
      return false;
    }
    m_component->activateBus(kAudio, kInput, 0, true);
    m_component->activateBus(kAudio, kOutput, 0, true);
    m_component->setActive(true);
    m_processor->setProcessing(true);

    for (auto *v : { &m_in0, &m_in1, &m_out0, &m_out1, &m_peerIn, &m_peerOut0, &m_peerOut1 }) v->assign(m_block, 0.0f);
    return true;
  }

  void unload()
  {
    if (m_processor) m_processor->setProcessing(false);
    if (m_component)
    {
      m_component->setActive(false);
      m_component->terminate();
    }
    m_commands = nullptr;
    m_processor = nullptr;
    m_component = nullptr;
    m_module = nullptr;
  }

  std::string command(const std::string &cmd)
  {
    std::vector<char> reply(4096);
    const int32 n = m_commands->runCommand(cmd.c_str(), reply.data(), (int32)reply.size());
    if (n >= (int32)reply.size())
    {
      reply.resize(n + 1);
      m_commands->runCommand(cmd.c_str(), reply.data(), (int32)reply.size()); // status, users, chatlog: no side effects
    }
    return std::string(reply.data());
  }

  std::map<std::string, std::string> status() { return parse_status(command("status")); }

  // one block of audio through the plug-in and the peer, in real time
  void block(NJClient *peer, bool record)
  {
    for (int i = 0; i < m_block; i ++)
    {
      m_in0[i] = m_in1[i] = (float)(0.5 * sin(2*PI*m_freq*(m_pos + i)/m_srate));
      m_peerIn[i] = (float)(0.5 * sin(2*PI*PEER_FREQ*(m_pos + i)/m_srate));
    }

    float *ins[2] = { m_in0.data(), m_in1.data() }, *outs[2] = { m_out0.data(), m_out1.data() };
    AudioBusBuffers inBus {}, outBus {};
    inBus.numChannels = 2;
    inBus.channelBuffers32 = ins;
    outBus.numChannels = 2;
    outBus.channelBuffers32 = outs;
    ProcessData data {};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = m_block;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inBus;
    data.outputs = &outBus;
    m_processor->process(data);
    if (record)
    {
      std::lock_guard<std::mutex> lock(m_heardMutex);
      m_pluginHeard.insert(m_pluginHeard.end(), m_out0.begin(), m_out0.end());
    }

    if (peer)
    {
      float *pin[2] = { m_peerIn.data(), m_peerIn.data() }, *pout[2] = { m_peerOut0.data(), m_peerOut1.data() };
      peer->AudioProc(pin, 1, pout, 2, m_block, m_srate);
      if (record) m_peerHeard.insert(m_peerHeard.end(), m_peerOut0.begin(), m_peerOut0.end());
      for (int i = 0; i < 4; i ++) peer->Run();
    }
    m_pos += m_block;

    // real time
    if (m_start == 0) m_start = now_seconds();
    const double due = m_start + (double)m_pos / m_srate;
    const double wait = due - now_seconds();
    if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
  }

  void run(NJClient *peer, double seconds, bool record = false)
  {
    const long blocks = (long)ceil(seconds * m_srate / m_block);
    for (long i = 0; i < blocks; i ++) block(peer, record);
  }

  // runs until pred() holds, at most timeout seconds
  template <typename F> bool runUntil(NJClient *peer, double timeout, F pred)
  {
    const double end = now_seconds() + timeout;
    while (now_seconds() < end)
    {
      if (pred()) return true;
      run(peer, 0.05);
    }
    return pred();
  }

  // what the plug-in played (recorded with block(..., true), maybe on another thread)
  std::vector<float> pluginHeard()
  {
    std::lock_guard<std::mutex> lock(m_heardMutex);
    return m_pluginHeard;
  }
  void clearHeard()
  {
    std::lock_guard<std::mutex> lock(m_heardMutex);
    m_pluginHeard.clear();
    m_peerHeard.clear();
  }

  int m_srate, m_block;
  double m_freq; // of the tone played into the plug-in
  long m_pos = 0;
  double m_start = 0;
  std::vector<float> m_pluginHeard, m_peerHeard;
  std::mutex m_heardMutex;

private:
  VST3::Hosting::Module::Ptr m_module;
  IPtr<HostApplication> m_hostApp;
  IPtr<IComponent> m_component;
  IPtr<IAudioProcessor> m_processor;
  IPtr<ninjam_vst3::INinjamCommand> m_commands;
  std::vector<float> m_in0, m_in1, m_out0, m_out1, m_peerIn, m_peerOut0, m_peerOut1;
};

static int fail(Host &host, const char *what)
{
  printf("error: %s\n", what);
  printf("status: %s\n", host.command("status").c_str());
  printf("chat:\n%s", host.command("chatlog").c_str());
  fflush(stdout);
  host.unload();
  return 1;
}

// the plug-in joins a server whose room plays Vorbis: it sends nothing and plays silence
static int join_vorbis_room(Host &host, const std::string &server)
{
  for (std::string c : { std::string("name plugin"), std::string("metronome off"), std::string("monitor off"), "connect " + server })
  {
    const std::string r = host.command(c);
    if (r != "ok") return fail(host, ("command failed: " + c + ": " + r).c_str());
  }
  if (!host.runUntil(nullptr, 15.0, [&]() { return host.status()["state"] == "connected"; }))
    return fail(host, "the plug-in did not join the server");
  printf("JOINED\n");

  bool go = false;
  if (!host.runUntil(nullptr, 30.0, [&]() {
        std::string line;
        if (stdin_has_line(line) && line == "go") go = true;
        auto s = host.status();
        return go && s["codec"] == "Vorbis" && atoi(s["users"].c_str()) >= 1;
      }))
    return fail(host, go ? "the room did not play Vorbis, or nobody else joined" : "no \"go\" from the test");

  // what the plug-in plays while the others send Vorbis (after the interval it may have
  // started in Opus, before they came)
  host.run(nullptr, 1.5);
  printf("PHASE vorbis BEGIN\n");
  host.m_pluginHeard.clear();
  host.run(nullptr, 6.0, true);
  float peak = 0;
  for (float v : host.m_pluginHeard) peak = std::max(peak, fabsf(v));
  auto st = host.status();
  const std::string chat = host.command("chatlog");
  const bool warned = chat.find("doesn't support Opus") != std::string::npos;
  std::string users = host.command("users");
  printf("users: %s", users.c_str());
  printf("PHASE vorbis END room=%s upload=%s peak=%.6f warned=%d others=%s\n", st["codec"].c_str(), st["upload"].c_str(), peak,
         warned ? 1 : 0, st["users"].c_str());

  host.command("disconnect");
  host.runUntil(nullptr, 5.0, [&]() { return host.status()["state"] != "connected"; });
  host.unload();
  printf("DONE\n");
  return 0;
}

// plays an instance's audio on a thread of its own until destroyed, as a DAW may for a second track
class AudioThread
{
public:
  explicit AudioThread(Host &host) : m_host(host), m_thread([this]() { while (!m_stop) m_host.block(nullptr, m_record); }) {}
  ~AudioThread()
  {
    m_stop = true;
    m_thread.join();
  }
  void record(bool on) { m_record = on; }

private:
  Host &m_host;
  std::atomic<bool> m_stop { false }, m_record { false };
  std::thread m_thread; // last: starts when the rest is ready
};

static bool contains(const std::string &s, const std::string &what) { return s.find(what) != std::string::npos; }

// two instances in this process (two tracks): one server between them, and they hear each other
static int jam_two_instances(Host &a, Host &b)
{
  for (const char *c : { "name alice", "metronome off", "monitor off", "host on" })
  {
    const std::string r = a.command(c);
    if (r != "ok") return fail(a, ("alice: command failed: " + std::string(c) + ": " + r).c_str());
  }
  for (const char *c : { "name bob", "metronome off", "monitor off" })
  {
    const std::string r = b.command(c);
    if (r != "ok") return fail(b, ("bob: command failed: " + std::string(c) + ": " + r).c_str());
  }

  int result = 0;
  {
    AudioThread bAudio(b); // bob's audio, from now on
    if (!a.runUntil(nullptr, 15.0, [&]() { auto s = a.status(); return s["state"] == "connected" && s["hosting"] == "1"; }))
      return fail(a, "alice did not start her server and join it");
    const std::string port = a.status()["port"];
    printf("PORT %s\n", port.c_str());
    a.command("chat !vote bpm 240");
    a.command("chat !vote bpi 4");

    // one server per process: bob is told to join alice's
    b.command("host on");
    const bool refused = a.runUntil(nullptr, 5.0, [&]() {
      return contains(b.command("chatlog"), "already hosts a server (port " + port + ")");
    }) && b.status()["hosting"] == "0";
    b.command("connect localhost:" + port);
    if (!a.runUntil(nullptr, 30.0, [&]() {
          auto sa = a.status(), sb = b.status();
          return sa["state"] == "connected" && sb["state"] == "connected" && atoi(sa["users"].c_str()) >= 1 &&
                 atoi(sb["users"].c_str()) >= 1 && sa["bpi"] == "4" && sb["bpi"] == "4" && atof(sb["bpm"].c_str()) == 240.0;
        }))
      return fail(b, "bob did not join alice's server, or the tempo did not change");
    a.command("chat hello from alice");

    const int ilen = (int)lround(60.0 / 240 * 4 * a.m_srate);
    if (!a.runUntil(nullptr, 10.0, [&]() { return atoi(a.status()["len"].c_str()) == ilen && atoi(b.status()["len"].c_str()) == ilen; }))
      return fail(a, "the intervals did not get to 1 second");
    const double interval = (double)ilen / a.m_srate;
    a.run(nullptr, interval * 2.2);
    printf("PHASE two BEGIN\n");
    a.clearHeard();
    b.clearHeard();
    bAudio.record(true);
    a.run(nullptr, interval * 4, true);
    bAudio.record(false);

    // the last two intervals each recorded
    const std::vector<float> ah = a.pluginHeard(), bh = b.pluginHeard();
    const size_t len = (size_t)ilen * 2;
    const double aSnr = tone_snr(ah, ah.size() > len ? ah.size() - len : 0, ah.size(), b.m_freq, a.m_srate);
    const double bSnr = tone_snr(bh, bh.size() > len ? bh.size() - len : 0, bh.size(), a.m_freq, b.m_srate);
    const bool gotChat = contains(b.command("chatlog"), "hello from alice");
    printf("users (alice): %s", a.command("users").c_str());
    printf("users (bob): %s", b.command("users").c_str());
    printf("PHASE two END a_hears_snr=%.1f b_hears_snr=%.1f b_host_refused=%d b_got_chat=%d a_users=%s b_users=%s\n", aSnr, bSnr,
           refused ? 1 : 0, gotChat ? 1 : 0, a.status()["users"].c_str(), b.status()["users"].c_str());

    // alice stops her server, which drops bob; now bob can host, and alice joins him
    a.command("host off");
    const bool dropped = a.runUntil(nullptr, 15.0, [&]() { return a.status()["hosting"] == "0" && b.status()["state"] != "connected"; });
    b.command("host on");
    a.runUntil(nullptr, 15.0, [&]() { auto s = b.status(); return s["state"] == "connected" && s["hosting"] == "1"; });
    const std::string port2 = b.status()["port"];
    a.command("connect localhost:" + port2);
    a.runUntil(nullptr, 15.0, [&]() { auto s = a.status(); return s["state"] == "connected" && atoi(s["users"].c_str()) >= 1; });
    auto sa = a.status();
    printf("PHASE swap END b_dropped=%d b_hosting=%s port=%s a_state=%s a_users=%s\n", dropped ? 1 : 0, b.status()["hosting"].c_str(),
           port2.c_str(), sa["state"].c_str(), sa["users"].c_str());

    b.command("host off");
    if (!a.runUntil(nullptr, 15.0, [&]() { return b.status()["hosting"] == "0" && a.status()["state"] != "connected"; }))
      result = fail(b, "bob's server did not stop");
  }
  b.unload();
  if (result) return result;
  a.unload();
  printf("DONE\n");
  return 0;
}

int main(int argc, char **argv)
{
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc < 2)
  {
    fprintf(stderr, "usage: %s path/to/NINJAM.vst3 [samplerate [blocksize [join host:port | two]]]\n", argv[0]);
    return 2;
  }
  const int srate = argc > 2 ? atoi(argv[2]) : 48000;
  const int blockSize = argc > 3 ? atoi(argv[3]) : 512;

  Host host(srate, blockSize);
  if (!host.load(argv[1])) return 1;
  if (argc > 5 && !strcmp(argv[4], "join")) return join_vorbis_room(host, argv[5]);
  if (argc > 4 && !strcmp(argv[4], "two"))
  {
    Host second(srate, blockSize, PEER_FREQ);
    if (!second.loadAnother(host)) return fail(host, "could not create a second instance");
    return jam_two_instances(host, second);
  }

  // the plug-in hosts a server and joins it
  for (const char *c : { "name plugin", "metronome off", "monitor off", "host on" })
  {
    const std::string r = host.command(c);
    if (r != "ok") return fail(host, ("command failed: " + std::string(c) + ": " + r).c_str());
  }
  if (!host.runUntil(nullptr, 15.0, [&]() { auto s = host.status(); return s["state"] == "connected" && s["hosting"] == "1"; }))
    return fail(host, "the plug-in did not start its server and join it");
  const std::string port = host.status()["port"];
  printf("PORT %s\n", port.c_str());

  // short intervals: a single vote passes on this server
  host.command("chat !vote bpm 240");
  host.command("chat !vote bpi 4");

  // the second client
  JNL::open_socketlib();
  NJClient peer;
  peer.config_metronome_mute = true;
  peer.config_metronome = 0.0f;
  peer.SetLocalChannelInfo(0, "tone", true, 0, true, 64, true, true);
  peer.SetLocalChannelMonitoring(0, false, 0.0f, false, 0.0f, true, true, false, false); // hear only the plug-in
  peer.Connect(("127.0.0.1:" + port).c_str(), "anonymous:peer", "");

  // wait for the watcher (test_plugin_e2e.py) and for the new tempo
  bool go = false;
  const bool ready = host.runUntil(&peer, 30.0, [&]() {
    std::string line;
    if (stdin_has_line(line) && line == "go") go = true;
    auto s = host.status();
    return go && peer.GetStatus() == NJClient::NJC_STATUS_OK && peer.GetBPI() == 4 &&
           s["bpi"] == "4" && atof(s["bpm"].c_str()) == 240.0 && atoi(s["users"].c_str()) >= 1;
  });
  if (!ready) return fail(host, go ? "the peer did not join, or the vote did not change the tempo" : "no \"go\" from the watcher");
  if (host.status()["kbps"] != "128") return fail(host, "the Opus bitrate is not 128 kbps by default");

  struct Phase
  {
    const char *name;
    const char *command; // what starts the phase
    const char *room, *fourcc, *kbps;
    int bpm, bpi, intervals; // tempo, and the intervals to record
  };
  const Phase phases[] = {
    { "opus", nullptr, "Opus", "OPUS", "128", 240, 4, 4 },
    { "flac", "chat !codec flac", "FLAC", "FLAC", "128", 240, 4, 4 },
    { "opus_again", "chat !codec opus", "Opus", "OPUS", "128", 240, 4, 4 },
    { "opus256", "bitrate 256", "Opus", "OPUS", "256", 240, 4, 4 },
    { "opus510", "bitrate 510", "Opus", "OPUS", "510", 240, 4, 4 },
    // long intervals at the highest bitrate: about 1 MB each. The peer's last short interval
    // may arrive just after the plug-in's interval starts, so the plug-in can play the peer
    // one interval later than the peer plays it (NINJAM's usual catch-up): record three.
    { "opus510_long", "chat !vote bpm 60", "Opus", "OPUS", "510", 60, 16, 3 },
  };
  int failures = 0;
  for (const Phase &ph : phases)
  {
    if (ph.command)
    {
      const std::string r = host.command(ph.command);
      if (r != "ok") return fail(host, ("command failed: " + std::string(ph.command) + ": " + r).c_str());
    }
    if (ph.bpi != 4) host.command("chat !vote bpi " + std::to_string(ph.bpi));
    const int ilen = (int)lround(60.0 / ph.bpm * ph.bpi * srate);
    if (!host.runUntil(&peer, 30.0, [&]() {
          auto s = host.status();
          return s["codec"] == ph.room && s["kbps"] == ph.kbps && atoi(s["len"].c_str()) == ilen;
        }))
      return fail(host, (std::string("the room did not get to ") + ph.name).c_str());

    // intervals that started before the change are still the old way (a new bitrate
    // applies from the next interval, and what each side plays is one interval behind)
    const double interval = (double)ilen / srate;
    host.run(&peer, ph.bpi == 4 ? interval * 2.2 : interval * 0.1);
    printf("PHASE %s BEGIN\n", ph.name);

    host.m_pluginHeard.clear();
    host.m_peerHeard.clear();
    host.run(&peer, interval * ph.intervals + (ph.bpi == 4 ? 0 : 1.0), true);
    auto st = host.status();

    // measure the last whole intervals (two, or one when they are long)
    const size_t n = host.m_pluginHeard.size(), len = (size_t)ilen * (ph.bpi == 4 ? 2 : 1);
    const size_t from = n > len ? n - len : 0;
    const double pluginSnr = tone_snr(host.m_pluginHeard, from, n, PEER_FREQ, srate);
    const double peerSnr = tone_snr(host.m_peerHeard, from, n, PLUGIN_FREQ, srate);
    if (getenv("NINJAM_E2E_DEBUG"))
    {
      std::string lv;
      for (size_t i = 0; i + srate <= n; i += srate)
      {
        double e = 0, f = 0;
        for (size_t j = i; j < i + srate; j ++) { e += host.m_pluginHeard[j] * host.m_pluginHeard[j]; f += host.m_peerHeard[j] * host.m_peerHeard[j]; }
        char b[64];
        snprintf(b, sizeof(b), " %.2f/%.2f", sqrt(e / srate), sqrt(f / srate));
        lv += b;
      }
      printf("levels (plugin/peer rms per second):%s\n", lv.c_str());
    }
    printf("PHASE %s END room=%s upload=%s kbps=%s plugin_hears_snr=%.1f peer_hears_snr=%.1f\n", ph.name,
           st["codec"].c_str(), st["upload"].c_str(), st["kbps"].c_str(), pluginSnr, peerSnr);
    if (st["upload"] != ph.fourcc) failures++;
  }

  printf("users: %s", host.command("users").c_str());
  host.command("host off");
  host.runUntil(&peer, 10.0, [&]() { return host.status()["hosting"] == "0"; });
  peer.Disconnect();
  host.unload();
  JNL::close_socketlib();
  printf("DONE\n");
  return failures ? 1 : 0;
}
