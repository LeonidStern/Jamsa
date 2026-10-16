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

  It loads Jamsa.vst3, plays a 997 Hz tone into it in real time, and drives
  it through its text command interface.

  Usage: plugin_e2e_host path/to/Jamsa.vst3 [samplerate [blocksize [join host:port | two | sync]]]

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
  process, the second with its audio on a thread of its own, both with the default name
  (so the server sees one name twice, from one address). "alice" hosts a server; "bob"
  can't host a second one in this process and is told to join, joins as the name's
  second player, and they hear each other (alice plays 997 Hz, bob 661 Hz) and stay in.
  Then alice stops her server, which drops bob; bob hosts one, and alice joins it.
    PORT <port>            alice's server is up
    PHASE two BEGIN
    PHASE two END a_hears_snr=<dB> b_hears_snr=<dB> b_host_refused=<0|1> b_got_chat=<0|1> a_users=<n> b_users=<n>
                  a_name=<alice's name> b_name=<bob's> a_sees=<who alice hears, as shown> b_sees=<who bob hears>
                  a_state=<state> b_state=<state> (at the end)
    PHASE swap END b_dropped=<0|1> b_hosting=<0|1> port=<port> a_state=<state> a_users=<n>
    DONE

  The host leads (sync): this host passes a tempo, a transport and a time signature (4/4) in
  the ProcessContext, as Live does, and the plug-in follows it ("Sync", on by default). The host is at
  100 BPM; the plug-in hosts a room (120 BPM) and votes it to 100 once it's in; the room votes 4
  beats. The host plays from the middle of bar 3: each interval must start on a host beat that
  is a multiple of 4 (the metronome's accent). The host's tempo is dragged to 110: one vote, and
  the intervals line up again. Someone votes the room to 90: no vote back, and a note says the
  tempos differ. At 90, the host loops 6 beats (intervals don't move), then loops 6 beats
  without saying so (no cycle in the ProcessContext: the first time round is a move, then
  they don't move), its playhead moves (they line up again), and it stops (they play on).
  From before it starts to when it stops, each interval the plug-in plays (the one it sends)
  is of the usual length, but one for each fix. With following off, the host at 120 votes
  nothing; turned on again, it votes 120.
    PORT <port>
    SYNC join room_bpm=<bpm> tempovotes=<n>
    SYNC phase <name> n=<accents> max_ms=<ms from a host beat that is a multiple of BPI> gap_err=<samples> fixes=<n> bars=<state>
                     accent_hz=<the accents' pitch> beat_hz=<the other beats'> beats=<other beats>
    NOTE next ppq=<the host's position> text=<where the editor says the next interval starts>
    SYNC follow room_bpm=<bpm> tempovotes=<n>
    NOTE other room_bpm=<bpm> tempovotes=<n> text=<the note>
    SYNC loop fixes_before=<n>             (then SYNC phase loop ...: measured as if the host never looped)
    SYNC hidden fixes_before=<n>           (then SYNC phase hidden ...: as if it never looped after the first time)
    SYNC stopped n=<accents> gap_err=<samples> fixes=<n> bars=<state>
    SYNC tape intervals=<n> odd=<intervals of another length than the usual> fixes=<n> odd_lengths=<samples,...>
    NOTE off room_bpm=<bpm> tempovotes=<n> bars=<state> text=<the note, with following off>
    SYNC refollow room_bpm=<bpm> tempovotes=<n>
    NOTE same text=<empty when the tempos agree>
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
#include "pluginterfaces/vst/ivstprocesscontext.h"

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
      if (ci.category() != kVstAudioEffectClass || ci.name() != "Jamsa") continue;
      m_component = factory.createInstance<IComponent>(ci.ID());
      break;
    }
    if (!m_component)
    {
      printf("error: no Jamsa audio effect class in the module\n");
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
    ProcessContext context {};
    const Transport played = m_transport; // the host's transport for this block
    if (played.on)
    {
      // the host's tempo and transport, as a DAW passes them
      context.sampleRate = m_srate;
      context.continousTimeSamples = m_pos;
      context.state = ProcessContext::kContTimeValid | ProcessContext::kProjectTimeMusicValid | ProcessContext::kTimeSigValid;
      context.projectTimeMusic = played.ppq;
      context.timeSigNumerator = 4;
      context.timeSigDenominator = 4;
      if (played.tempo > 0)
      {
        context.tempo = played.tempo;
        context.state |= ProcessContext::kTempoValid;
      }
      if (played.playing) context.state |= ProcessContext::kPlaying;
      if (played.cycle && played.tellCycle)
      {
        context.cycleStartMusic = played.cycleStart;
        context.cycleEndMusic = played.cycleEnd;
        context.state |= ProcessContext::kCycleActive | ProcessContext::kCycleValid;
      }
      data.processContext = &context;
      if (played.playing)
      {
        const double beats = m_block / (double)m_srate * played.tempo / 60.0;
        m_transport.ppq += beats;
        m_transport.line += beats;
        // the loop goes back at the end of a block (the next block starts in the loop)
        if (played.cycle && m_transport.ppq >= played.cycleEnd)
        {
          m_transport.ppq -= played.cycleEnd - played.cycleStart;
          // a loop the host doesn't declare is a move for the plug-in, the first time round
          if (m_transport.wraps++ == 0 && !played.tellCycle) m_transport.line = m_transport.ppq;
        }
      }
    }
    data.symbolicSampleSize = kSample32;
    data.numSamples = m_block;
    data.numInputs = 1;
    data.numOutputs = 1;
    data.inputs = &inBus;
    data.outputs = &outBus;
    m_processor->process(data);
    if (record || m_taping)
    {
      std::lock_guard<std::mutex> lock(m_heardMutex);
      if (record)
      {
        m_pluginHeard.insert(m_pluginHeard.end(), m_out0.begin(), m_out0.end());
        m_heardTransport.push_back(played);
      }
      if (m_taping) m_tape.insert(m_tape.end(), m_out0.begin(), m_out0.end());
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
    m_heardTransport.clear();
  }

  // the host's transport (sync), changed on the thread that plays the blocks
  struct Transport
  {
    bool on = false; // pass a ProcessContext
    double tempo = 0;
    bool playing = false;
    double ppq = 0;  // at the next block
    double line = 0; // the same, as if the host never looped since it last moved
    bool cycle = false;
    double cycleStart = 0, cycleEnd = 0;
    bool tellCycle = true; // false: the host loops without saying so
    int wraps = 0;         // times round the loop
  };
  Transport m_transport;
  void locate(double ppq) { m_transport.ppq = m_transport.line = ppq; }
  std::vector<Transport> m_heardTransport; // for each block of m_pluginHeard

  int m_srate, m_block;
  double m_freq; // of the tone played into the plug-in
  long m_pos = 0;
  double m_start = 0;
  std::vector<float> m_pluginHeard, m_peerHeard;
  bool m_taping = false;     // all the plug-in plays goes on m_tape too, recorded or not
  std::vector<float> m_tape;
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
  // both keep the default name
  for (const char *c : { "metronome off", "monitor off", "host on" })
  {
    const std::string r = a.command(c);
    if (r != "ok") return fail(a, ("alice: command failed: " + std::string(c) + ": " + r).c_str());
  }
  for (const char *c : { "metronome off", "monitor off" })
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
    auto shown = [](const std::string &users) {
      const size_t at = users.find("shown=");
      return at == std::string::npos ? std::string("-") : users.substr(at + 6, users.find('\n', at) - at - 6);
    };
    auto sa = a.status(), sb = b.status();
    printf("PHASE two END a_hears_snr=%.1f b_hears_snr=%.1f b_host_refused=%d b_got_chat=%d a_users=%s b_users=%s a_name=%s b_name=%s "
           "a_sees=%s b_sees=%s a_state=%s b_state=%s\n",
           aSnr, bSnr, refused ? 1 : 0, gotChat ? 1 : 0, sa["users"].c_str(), sb["users"].c_str(), sa["user"].c_str(), sb["user"].c_str(),
           shown(a.command("users")).c_str(), shown(b.command("users")).c_str(), sa["state"].c_str(), sb["state"].c_str());

    // alice stops her server, which drops bob; now bob can host, and alice joins him
    a.command("host off");
    const bool dropped = a.runUntil(nullptr, 15.0, [&]() { return a.status()["hosting"] == "0" && b.status()["state"] != "connected"; });
    b.command("host on");
    a.runUntil(nullptr, 15.0, [&]() { auto s = b.status(); return s["state"] == "connected" && s["hosting"] == "1"; });
    const std::string port2 = b.status()["port"];
    a.command("connect localhost:" + port2);
    a.runUntil(nullptr, 15.0, [&]() { auto s = a.status(); return s["state"] == "connected" && atoi(s["users"].c_str()) >= 1; });
    sa = a.status();
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

// The metronome's beats in what the plug-in played since clearHeard(): the first sample that
// isn't silent after 50 ms of silence (nothing else plays). The accents (the first beat of each
// interval) peak at 0.4 or more in their first 10 ms, the other beats lower.
static std::vector<long> metronome_beats(const std::vector<float> &x, int srate, bool accents)
{
  std::vector<long> found;
  long quietSince = 0;
  for (size_t i = 0; i < x.size(); i ++)
  {
    if (fabsf(x[i]) < 1e-6f) continue;
    const bool onset = (long)i - quietSince >= srate / 20;
    quietSince = (long)i + 1;
    if (!onset || i == 0) continue;
    float peak = 0;
    for (size_t j = i; j < std::min(x.size(), i + srate / 100); j ++) peak = std::max(peak, fabsf(x[j]));
    if (peak >= 0.1f && (peak >= 0.4f) == accents) found.push_back((long)i);
  }
  return found;
}

// the pitch of the click at x[i]: zero crossings in its first 20 ms
static double click_hz(const std::vector<float> &x, long i, int srate)
{
  const long n = srate / 50;
  int crossings = 0;
  for (long j = i + 1; j < std::min<long>((long)x.size(), i + n); j ++)
    if ((x[j - 1] < 0) != (x[j] < 0) && x[j] != 0 && x[j - 1] != 0) crossings++;
  return crossings / 2.0 / 0.02;
}

// How far each accent since clearHeard() is from a host beat that is a multiple of bpi (as if
// the host never looped), and how far the gaps between accents are from the usual interval.
static void measure_sync(Host &host, const char *name, bool playing)
{
  std::vector<float> x;
  std::vector<Host::Transport> tr;
  {
    std::lock_guard<std::mutex> lock(host.m_heardMutex);
    x = host.m_pluginHeard;
    tr = host.m_heardTransport;
  }
  auto s = host.status();
  const int bpi = atoi(s["bpi"].c_str()), len = atoi(s["len"].c_str());
  const std::vector<long> acc = metronome_beats(x, host.m_srate, true), beats = metronome_beats(x, host.m_srate, false);
  double accentHz = 0, beatHz = 0;
  for (long i : acc) accentHz += click_hz(x, i, host.m_srate) / acc.size();
  for (long i : beats) beatHz += click_hz(x, i, host.m_srate) / beats.size();
  double worst = 0;
  long gapErr = 0;
  std::string list;
  for (size_t k = 0; k < acc.size(); k ++)
  {
    if (k > 0) gapErr = std::max(gapErr, labs(acc[k] - acc[k - 1] - len));
    if (!playing) continue;
    const Host::Transport &t = tr[acc[k] / host.m_block];
    const double beat = t.line + (acc[k] % host.m_block) / (double)host.m_srate * t.tempo / 60.0;
    double off = fmod(beat, bpi);
    if (off > bpi / 2.0) off -= bpi;
    const double ms = off * 60000.0 / t.tempo;
    worst = std::max(worst, fabs(ms));
    char b[32];
    snprintf(b, sizeof(b), "%s%.3f", list.empty() ? "" : ",", ms);
    list += b;
  }
  if (playing) printf("accents (ms from a host beat that is a multiple of %d): %s\n", bpi, list.c_str());
  if (playing)
    printf("SYNC phase %s n=%d max_ms=%.3f gap_err=%ld fixes=%s bars=%s accent_hz=%.0f beat_hz=%.0f beats=%d\n", name, (int)acc.size(), worst,
           gapErr, s["barfixes"].c_str(), s["bars"].c_str(), accentHz, beatHz, (int)beats.size());
  else
    printf("SYNC %s n=%d gap_err=%ld fixes=%s bars=%s\n", name, (int)acc.size(), gapErr, s["barfixes"].c_str(), s["bars"].c_str());
}

// The intervals the plug-in played on the tape (the gaps between its accents), which are the
// ones it sent: how many there are, and the lengths of those that aren't one of `usual`.
static int odd_intervals(const std::vector<float> &x, int srate, const std::vector<int> &usual, std::string &oddLengths)
{
  const std::vector<long> acc = metronome_beats(x, srate, true);
  for (size_t k = 1; k < acc.size(); k ++)
  {
    const long gap = acc[k] - acc[k - 1];
    if (std::find(usual.begin(), usual.end(), (int)gap) != usual.end()) continue;
    oddLengths += (oddLengths.empty() ? "" : ",") + std::to_string(gap);
  }
  return acc.empty() ? 0 : (int)acc.size() - 1;
}

// the host leads: the room follows its tempo, and the intervals its bars
static int host_sync(Host &host)
{
  Host::Transport &t = host.m_transport;
  t.on = true;
  t.tempo = 100;
  auto len_at = [&](int bpm, int bpi) { return (int)(60.0 / bpm * bpi * host.m_srate); };
  auto len_is = [&](int bpm) { return atoi(host.status()["len"].c_str()) == len_at(bpm, 4); };
  auto bars_are = [&](const char *state) { return host.status()["bars"] == state; };
  // records `intervals` intervals once the plug-in says they're in line
  auto record = [&](const char *name, int intervals) {
    host.run(nullptr, 0.1); // the plug-in sees the host's last change
    if (!host.runUntil(nullptr, 20.0, [&]() { return bars_are("aligned"); })) return false;
    host.clearHeard();
    host.run(nullptr, atoi(host.status()["len"].c_str()) / (double)host.m_srate * intervals + 0.2, true);
    measure_sync(host, name, true);
    return true;
  };

  for (const char *c : { "name plugin", "metronome on", "monitor off", "host on" })
  {
    const std::string r = host.command(c);
    if (r != "ok") return fail(host, ("command failed: " + std::string(c) + ": " + r).c_str());
  }
  if (!host.runUntil(nullptr, 15.0, [&]() { auto s = host.status(); return s["state"] == "connected" && s["hosting"] == "1"; }))
    return fail(host, "the plug-in did not start its server and join it");
  printf("PORT %s\n", host.status()["port"].c_str());

  // in the room (120 BPM), the plug-in votes it to the host's 100
  if (!host.runUntil(nullptr, 15.0, [&]() { return atof(host.status()["bpm"].c_str()) == 100.0; }))
    return fail(host, "the room did not follow the host's tempo when the plug-in joined");
  if (host.command("vote bpi 4") != "ok") return fail(host, "vote bpi 4 failed");
  if (!host.runUntil(nullptr, 15.0, [&]() { return len_is(100); })) return fail(host, "the room did not get to 4 beats");
  auto s = host.status();
  printf("SYNC join room_bpm=%s tempovotes=%s\n", s["bpm"].c_str(), s["tempovotes"].c_str());

  // from an interval before the host plays to when it stops, the tape has each interval the plug-in sent
  host.m_taping = true;
  host.run(nullptr, len_at(100, 4) / (double)host.m_srate + 0.3);

  // the host plays from the middle of bar 3
  host.locate(9.37);
  t.playing = true;
  if (!record("100bpm", 4)) return fail(host, "the intervals did not line up with the host's bars at 100 BPM");
  {
    const double ppq = t.ppq;
    printf("NOTE next ppq=%.3f text=%s\n", ppq, host.command("nextinterval").c_str());
  }

  // the host's tempo is dragged to 110: one vote, and the bars line up again
  for (double bpm : { 104.0, 106.5, 108.0, 110.0 })
  {
    t.tempo = bpm;
    host.run(nullptr, 0.15);
  }
  if (!host.runUntil(nullptr, 20.0, [&]() { return len_is(110); })) return fail(host, "the room did not follow the host's tempo to 110");
  s = host.status();
  printf("SYNC follow room_bpm=%s tempovotes=%s\n", s["bpm"].c_str(), s["tempovotes"].c_str());
  if (!record("110bpm", 3)) return fail(host, "the intervals did not line up with the host's bars at 110 BPM");

  // someone else votes the room to 90: the plug-in doesn't vote back, and says the tempos differ
  host.command("chat !vote bpm 90");
  if (!host.runUntil(nullptr, 20.0, [&]() { return len_is(90); })) return fail(host, "the room did not go to 90");
  host.run(nullptr, 2.0);
  if (!host.runUntil(nullptr, 10.0, [&]() { return !host.command("syncnote").empty(); })) return fail(host, "no note on the tempos");
  s = host.status();
  printf("NOTE other room_bpm=%s tempovotes=%s text=%s\n", s["bpm"].c_str(), s["tempovotes"].c_str(), host.command("syncnote").c_str());

  // the player sets the host to the room's 90; it loops 6 beats: the intervals play on as before
  t.tempo = 90;
  t.cycle = true;
  t.cycleStart = 8;
  t.cycleEnd = 14;
  host.locate(8);
  host.run(nullptr, 0.1);
  if (!host.runUntil(nullptr, 20.0, [&]() { return bars_are("aligned"); })) return fail(host, "the intervals did not line up in the loop");
  printf("SYNC loop fixes_before=%s\n", host.status()["barfixes"].c_str());
  host.clearHeard();
  host.run(nullptr, 4 * (6 * 60.0 / 90), true); // four times round the loop
  measure_sync(host, "loop", true);

  // the host loops 6 beats without saying so (no cycle in the ProcessContext), from a beat
  // before the loop: the first time round is a move (in line from there), then the same jump
  // back each time is a loop, and the intervals play on
  t.tellCycle = false;
  t.cycleStart = 40;
  t.cycleEnd = 46;
  t.wraps = 0;
  host.locate(37);
  if (!host.runUntil(nullptr, 20.0, [&]() { return t.wraps > 0; })) return fail(host, "the host did not loop");
  host.run(nullptr, 0.1);
  if (!host.runUntil(nullptr, 20.0, [&]() { return bars_are("aligned"); }))
    return fail(host, "the intervals did not line up in the loop the host doesn't declare");
  printf("SYNC hidden fixes_before=%s\n", host.status()["barfixes"].c_str());
  host.clearHeard();
  host.run(nullptr, 4 * (6 * 60.0 / 90), true);
  measure_sync(host, "hidden", true);
  t.tellCycle = true;

  // the playhead moves: in line again
  t.cycle = false;
  host.locate(101.5);
  if (!record("moved", 3)) return fail(host, "the intervals did not line up after the playhead moved");

  // stopped: the intervals play on
  t.playing = false;
  host.run(nullptr, 0.2);
  host.clearHeard();
  host.run(nullptr, len_at(90, 4) / (double)host.m_srate * 3 + 0.2, true);
  measure_sync(host, "stopped", false);

  // what the others heard: one interval of another length for each fix, the others all as usual
  host.m_taping = false;
  std::string oddLengths;
  const int intervals = odd_intervals(host.m_tape, host.m_srate, { len_at(100, 4), len_at(110, 4), len_at(90, 4) }, oddLengths);
  const int odd = oddLengths.empty() ? 0 : (int)std::count(oddLengths.begin(), oddLengths.end(), ',') + 1;
  printf("SYNC tape intervals=%d odd=%d fixes=%s odd_lengths=%s\n", intervals, odd, host.status()["barfixes"].c_str(),
         oddLengths.empty() ? "-" : oddLengths.c_str());
  host.m_tape.clear();

  // following off: the host at 120 votes nothing; on again, it votes 120
  host.command("follow off");
  t.tempo = 120;
  host.run(nullptr, 2.0);
  if (!host.runUntil(nullptr, 10.0, [&]() { return !host.command("syncnote").empty(); })) return fail(host, "no note with following off");
  s = host.status();
  printf("NOTE off room_bpm=%s tempovotes=%s bars=%s text=%s\n", s["bpm"].c_str(), s["tempovotes"].c_str(), s["bars"].c_str(),
         host.command("syncnote").c_str());
  host.command("follow on");
  if (!host.runUntil(nullptr, 20.0, [&]() { return len_is(120); })) return fail(host, "following again did not vote the room to 120");
  s = host.status();
  printf("SYNC refollow room_bpm=%s tempovotes=%s\n", s["bpm"].c_str(), s["tempovotes"].c_str());
  host.run(nullptr, 0.5);
  printf("NOTE same text=%s\n", host.command("syncnote").c_str());

  host.command("host off");
  host.runUntil(nullptr, 10.0, [&]() { return host.status()["hosting"] == "0"; });
  host.unload();
  printf("DONE\n");
  return 0;
}

int main(int argc, char **argv)
{
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc < 2)
  {
    fprintf(stderr, "usage: %s path/to/Jamsa.vst3 [samplerate [blocksize [join host:port | two | sync]]]\n", argv[0]);
    return 2;
  }
  const int srate = argc > 2 ? atoi(argv[2]) : 48000;
  const int blockSize = argc > 3 ? atoi(argv[3]) : 512;

  Host host(srate, blockSize);
  if (!host.load(argv[1])) return 1;
  if (argc > 5 && !strcmp(argv[4], "join")) return join_vorbis_room(host, argv[5]);
  if (argc > 4 && !strcmp(argv[4], "sync")) return host_sync(host);
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

  // short intervals: a single vote passes on this server (as the editor's BPM and BPI boxes send them)
  host.command("vote bpm 240");
  host.command("vote bpi 4");

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
