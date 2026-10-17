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

  Usage: plugin_e2e_host path/to/Jamsa.vst3 [samplerate [blocksize [join host:port | two | sync | link | live]]]

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
  Then bob changes the room's bitrate and codec, which changes alice's too. Last, alice
  stops her server, which drops bob; bob hosts one (at his bitrate), and alice joins it.
    PORT <port>            alice's server is up
    PHASE two BEGIN
    PHASE two END a_hears_snr=<dB> b_hears_snr=<dB> b_host_refused=<0|1> b_got_chat=<0|1> a_users=<n> b_users=<n>
                  a_name=<alice's name> b_name=<bob's> a_sees=<who alice hears, as shown> b_sees=<who bob hears>
                  a_state=<state> b_state=<state> (at the end) a_link=<alice's Link state> b_link=<bob's>
                  b_after_ms=<how long after alice's intervals bob's start, about half an interval>
                  a_player1_snr=<dB> (bob on alice's output Player 1)
                  a_other_players_peak=<the loudest sample on her other player outputs>
    PHASE room END start_kbps=<the room's at first> a_kbps=<alice sends> b_kbps=<bob sends> a_own=<alice's own>
                   b_own=<bob's own> b_saw_note=<0|1> flac=<alice's room>/<alice sends> opus=<the same, after>
                   start_bpi=<the room's BPI at first> least_bpi=<after a vote for 2>
    PHASE swap END b_dropped=<0|1> b_hosting=<0|1> port=<port> a_state=<state> a_users=<n> a_kbps=<alice sends>
                   a_link=<alice's Link state> b_link=<bob's>
    DONE
  Both have Sync on, so both are in Link while in a room, and the one in a room first leads
  it ("leading"), the other not ("other"); when alice leaves her room, bob leads it from his.

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
  No other Link app is there, so the plug-in is alone in Link, and its note says to turn
  Link on in the host.

  Ableton Link (link): this host is a Link peer too, and does what Live does with its Link
  on: its tempo (ProcessContext) is the Link session's, and while it plays its position is
  Link's beat when the block is heard, which is LATENCY_MS after the plug-in's guess (one
  block after process() is called). Its Link session is older and at 133 BPM. The plug-in
  hosts a room (120 BPM) and leads Link: the host takes the room's 120 (no vote for its 133),
  and the room's vote for 100 BPM and 4 beats. While it plays, each interval must start on a
  host beat that is a multiple of 4 (the accent of the plug-in's click, against the position
  the host passed), the plug-in must find the latency, and the intervals are never moved
  (Link moves the host's bars instead). The host's tempo is dragged to 109.6: one vote, and
  the room and Link at 110. The host's tempo goes to 110.3, which rounds to the room's: Link
  goes back to 110. Another app sets Link to 125 while the host's own Link is off: Link goes
  back to 110. Then the host's Link goes off: the plug-in says to turn it on, and lines its
  intervals up with the host's bars itself; on again, the plug-in leads it again. Last, Sync
  off: the plug-in leaves Link; and Sync on, out of the room: the plug-in leaves Link too.
    PORT <port>
    LINK nopeers                                       (no Link peer seen: the rest is skipped)
    LINK joined room_bpm=<bpm> peer_tempo=<bpm> tempovotes=<n> leads=<0|1> link=<state> restores=<n>
    LINK vote room_bpm=<bpm> peer_tempo=<bpm>
    LINK timing late_min=<ms> late_mean=<ms> late_max=<ms>  (how late this host called process(), while it played)
    SYNC phase link100 ... latency=<ms the plug-in measured>  (as in sync, on the host's position)
    LINK follow room_bpm=<bpm> peer_tempo=<bpm> tempovotes=<n>
    SYNC phase link110 ...
    LINK same peer_tempo=<bpm> tempovotes=<n> restores=<n>
    LINK foreign peer_tempo=<bpm> restores=<n>
    SYNC tape intervals=<n> odd=<n> fixes=<n> odd_lengths=<...>
    LINK off peers=<n> leads=<0|1> bars=<state> hint=<0|1> tip=<the Sync button's tooltip>
    LINK back peers=<n> leads=<0|1> peer_tempo=<bpm> tempovotes=<n>
    LINK syncoff link=<state> peer_peers=<n>
    LINK left link=<state> state=<the plug-in's connection> peer_peers_on=<n, Sync on again> peer_peers=<n, out of the room>
    chat: ...                                          (the plug-in's chat, a line each)
    DONE

  With NINJAM_E2E_DEBUG set, each phase also prints the level of what each side heard, second
  by second. NINJAM_E2E_JITTER_MS=<ms> makes this host call process() late as a busy machine
  does (CI's virtual machines): up to that much after each block, and five times that every
  two seconds.

*/

#include <ableton/Link.hpp> // first: it includes asio

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
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

    // the main output and a stereo output for each player, as Live asks for them
    SpeakerArrangement in = SpeakerArr::kStereo, outs[1 + PLAYER_OUTS];
    for (auto &o : outs) o = SpeakerArr::kStereo;
    if (m_processor->setBusArrangements(&in, 1, outs, 1 + PLAYER_OUTS) != kResultTrue)
    {
      printf("error: the plug-in refused a stereo in, stereo outs arrangement\n");
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
    for (int b = 0; b <= PLAYER_OUTS; b ++) m_component->activateBus(kAudio, kOutput, b, true);
    m_component->setActive(true);
    m_processor->setProcessing(true);

    for (auto *v : { &m_in0, &m_in1, &m_out0, &m_out1, &m_peerIn, &m_peerOut0, &m_peerOut1 }) v->assign(m_block, 0.0f);
    m_playerOut.assign(2 * PLAYER_OUTS, std::vector<float>(m_block, 0.0f));
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
      m_in0[i] = m_in1[i] = m_signal ? m_signal(m_pos + i) : (float)(0.5 * sin(2*PI*m_freq*(m_pos + i)/m_srate));
      m_peerIn[i] = (float)(0.5 * sin(2*PI*PEER_FREQ*(m_pos + i)/m_srate));
    }

    float *ins[2] = { m_in0.data(), m_in1.data() }, *outs[2] = { m_out0.data(), m_out1.data() };
    AudioBusBuffers inBus {}, outBus[1 + PLAYER_OUTS] {};
    inBus.numChannels = 2;
    inBus.channelBuffers32 = ins;
    outBus[0].numChannels = 2;
    outBus[0].channelBuffers32 = outs;
    float *playerOuts[2 * PLAYER_OUTS];
    for (int b = 0; b < PLAYER_OUTS; b ++)
    {
      playerOuts[2 * b] = m_playerOut[2 * b].data();
      playerOuts[2 * b + 1] = m_playerOut[2 * b + 1].data();
      outBus[1 + b].numChannels = 2;
      outBus[1 + b].channelBuffers32 = playerOuts + 2 * b;
    }
    ProcessData data {};
    data.processMode = kRealtime;
    ProcessContext context {};
    if (m_start == 0) m_start = now_seconds();
    if (m_link && m_linkBaseUs == 0) m_linkBaseUs = linkClockAtStart();
    if (m_link && m_transport.followLink)
    {
      // as Live with its Link on: the session's tempo, and while it plays, Link's beat when the
      // block is heard, by the sample count (a sound card's clock)
      const auto state = m_link->captureAppSessionState();
      m_transport.tempo = state.tempo();
      const double heardUs = m_linkBaseUs + (m_pos + m_block) * 1e6 / m_srate + m_latencyUs;
      m_transport.ppq = m_transport.line = state.beatAtTime(std::chrono::microseconds((long long)llround(heardUs)), 4.0);
    }
    const Transport played = m_transport; // the host's transport for this block
    if (m_link && played.playing)
    {
      // how late process() is called, after the block's time by the sample count
      const double late = ((double)m_link->clock().micros().count() - m_linkBaseUs - m_pos * 1e6 / m_srate) / 1000.0;
      m_lateMin = std::min(m_lateMin, late);
      m_lateMax = std::max(m_lateMax, late);
      m_lateSum += late;
      m_lateN++;
    }
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
      if (played.playing && !played.followLink)
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
    data.numOutputs = 1 + PLAYER_OUTS;
    data.inputs = &inBus;
    data.outputs = outBus;
    const double called = now_seconds();
    m_processor->process(data);
    m_maxProcessMs = std::max(m_maxProcessMs, (now_seconds() - called) * 1000);
    if (record || m_taping)
    {
      std::lock_guard<std::mutex> lock(m_heardMutex);
      if (record)
      {
        if (m_pluginHeard.empty()) m_heardFrom = m_pos;
        m_pluginHeard.insert(m_pluginHeard.end(), m_out0.begin(), m_out0.end());
        if (m_recordPlayers)
          for (int p = 0; p < PLAYER_OUTS; p ++)
            m_playersHeard[p].insert(m_playersHeard[p].end(), m_playerOut[2 * p].begin(), m_playerOut[2 * p].end());
        m_heardTransport.push_back(played);
        m_player1Heard.insert(m_player1Heard.end(), m_playerOut[0].begin(), m_playerOut[0].end());
        for (int c = 2; c < 2 * PLAYER_OUTS; c ++)
          for (float f : m_playerOut[c]) m_otherPlayersPeak = std::max(m_otherPlayersPeak, fabsf(f));
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
    const double due = m_start + (double)m_pos / m_srate;
    const double wait = due - now_seconds();
    if (wait > 0) std::this_thread::sleep_for(std::chrono::duration<double>(wait));
    // as a sound card after a stall (the test analysing audio): on from now, without the blocks
    // it missed in a burst
    else if (m_noCatchUp && wait < -0.02)
    {
      m_start -= wait;
      m_skips++;
      m_skippedMs += -wait * 1000;
    }
    if (m_jitterMs > 0)
    {
      // late, as on a busy machine: a little after each block, and more now and then
      m_rng = m_rng * 1103515245u + 12345u;
      double ms = m_jitterMs * ((m_rng >> 8) & 0xffff) / 65536.0;
      if (m_pos / (2 * m_srate) != (m_pos - m_block) / (2 * m_srate)) ms += 5 * m_jitterMs;
      std::this_thread::sleep_for(std::chrono::duration<double>(ms / 1000.0));
    }
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
  // what the output Player 1 played (left), and the loudest sample on the other player outputs
  std::vector<float> player1Heard()
  {
    std::lock_guard<std::mutex> lock(m_heardMutex);
    return m_player1Heard;
  }
  float otherPlayersPeak()
  {
    std::lock_guard<std::mutex> lock(m_heardMutex);
    return m_otherPlayersPeak;
  }
  // The DAW doesn't call process() for `blocks` blocks (busy elsewhere) while the caller lets
  // that time pass, and the sound card plays silence for them: the timeline goes on, so the
  // next block() is on time again
  void miss(int blocks, bool record)
  {
    const size_t n = (size_t)blocks * m_block;
    if (record)
    {
      std::lock_guard<std::mutex> lock(m_heardMutex);
      if (m_pluginHeard.empty()) m_heardFrom = m_pos;
      m_pluginHeard.insert(m_pluginHeard.end(), n, 0.0f);
      if (m_recordPlayers)
        for (auto &p : m_playersHeard) p.insert(p.end(), n, 0.0f);
      m_player1Heard.insert(m_player1Heard.end(), n, 0.0f);
      m_peerHeard.insert(m_peerHeard.end(), n, 0.0f);
      for (int i = 0; i < blocks; i ++) m_heardTransport.push_back(m_transport);
    }
    m_pos += (long)n;
  }

  void clearHeard()
  {
    std::lock_guard<std::mutex> lock(m_heardMutex);
    m_pluginHeard.clear();
    m_player1Heard.clear();
    m_otherPlayersPeak = 0;
    m_peerHeard.clear();
    m_heardTransport.clear();
    for (auto &p : m_playersHeard) p.clear();
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
    bool followLink = false; // tempo and position from m_link (link)
  };
  Transport m_transport;
  // link: the host's own Link, Link's clock at sample 0, and how much later than the plug-in's
  // guess a block is heard
  ableton::Link *m_link = nullptr;
  double m_linkBaseUs = 0, m_latencyUs = 0;
  // when this host called process() late, after the block's time (ms), while it played
  double m_lateMin = 1e9, m_lateMax = -1e9, m_lateSum = 0;
  long m_lateN = 0;
  void resetLate()
  {
    m_lateMin = 1e9;
    m_lateMax = -1e9;
    m_lateSum = 0;
    m_lateN = 0;
  }
  // m_start (sample 0) on Link's clock: the two clocks read together, the closest of a few tries
  double linkClockAtStart()
  {
    double best = 1e18, delta = 0;
    for (int i = 0; i < 5; i ++)
    {
      const double a = now_seconds() * 1e6, l = (double)m_link->clock().micros().count(), b = now_seconds() * 1e6;
      if (b - a < best)
      {
        best = b - a;
        delta = l - (a + b) / 2;
      }
    }
    return m_start * 1e6 + delta;
  }
  void locate(double ppq) { m_transport.ppq = m_transport.line = ppq; }
  std::vector<Transport> m_heardTransport; // for each block of m_pluginHeard

  int m_srate, m_block;
  double m_freq; // of the tone played into the plug-in
  std::function<float(long)> m_signal; // what the plug-in gets instead of the tone, by sample position
  long m_heardFrom = 0;                // the position of m_pluginHeard[0] (and m_peerHeard[0], m_playersHeard[p][0])
  bool m_recordPlayers = false;        // record each player output's left channel too (live)
  std::vector<float> m_playersHeard[8];
  bool m_noCatchUp = false;            // after a stall, go on in real time from now (live)
  int m_skips = 0;                     // how often it did, and the time it skipped in all
  double m_maxProcessMs = 0;           // the longest process() call
  double m_skippedMs = 0;
  long m_pos = 0;
  double m_start = 0;
  double m_jitterMs = getenv("NINJAM_E2E_JITTER_MS") ? atof(getenv("NINJAM_E2E_JITTER_MS")) : 0.0;
  unsigned int m_rng = 1;
  std::vector<float> m_pluginHeard, m_peerHeard;
  static constexpr int PLAYER_OUTS = 8; // Engine::NUM_PLAYER_OUTS
  std::vector<std::vector<float>> m_playerOut;
  std::vector<float> m_player1Heard;
  float m_otherPlayersPeak = 0;
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
    // the room starts at 4 BPI, and goes down to 2
    a.runUntil(nullptr, 5.0, [&]() { return atoi(a.status()["bpi"].c_str()) > 0; });
    const std::string startBpi = a.status()["bpi"];
    a.command("chat !vote bpi 2");
    a.runUntil(nullptr, 5.0, [&]() { return a.status()["bpi"] == "2"; });
    const std::string leastBpi = a.status()["bpi"];
    a.command("chat !vote bpm 240");
    a.command("chat !vote bpi 4");

    // one server per process: bob is told to join alice's
    b.command("host on");
    const bool refused = a.runUntil(nullptr, 5.0, [&]() {
      return contains(b.command("chatlog"), "already hosts a server (port " + port + ")");
    }) && b.status()["hosting"] == "0";
    // Bob joins halfway into one of alice's 1 second intervals (240 BPM and 4 BPI, from her next
    // one on), so that his start half an interval after hers. Each instance plays the other's
    // interval from its own next one that starts once the first 8 KB of it are in (NJClient's
    // prebuffer: at 510 kbps that is 130 ms of audio, in about 200 ms after it started). Joining
    // right after the votes, bob's intervals started 140 to 220 ms after alice's (the steps from her
    // joining to there take about as long each time): about when hers got in. The one time hers
    // got in a little later than his started, he had none to play, an interval of silence, and
    // played hers an interval later from then on.
    const int ilen = (int)lround(60.0 / 240 * 4 * a.m_srate);
    a.runUntil(nullptr, 5.0, [&]() {
      auto s = a.status();
      const int pos = atoi(s["pos"].c_str());
      return atoi(s["len"].c_str()) == ilen && pos >= ilen / 2 && pos < ilen * 5 / 8;
    });
    b.command("connect localhost:" + port);
    if (!a.runUntil(nullptr, 30.0, [&]() {
          auto sa = a.status(), sb = b.status();
          return sa["state"] == "connected" && sb["state"] == "connected" && atoi(sa["users"].c_str()) >= 1 &&
                 atoi(sb["users"].c_str()) >= 1 && sa["bpi"] == "4" && sb["bpi"] == "4" && atof(sb["bpm"].c_str()) == 240.0 &&
                 sb["link"] == "other";
        }))
      return fail(b, "bob did not join alice's server, or the tempo did not change");
    a.command("chat hello from alice");

    if (!a.runUntil(nullptr, 10.0, [&]() { return atoi(a.status()["len"].c_str()) == ilen && atoi(b.status()["len"].c_str()) == ilen; }))
      return fail(a, "the intervals did not get to 1 second");
    const double interval = (double)ilen / a.m_srate;
    a.run(nullptr, interval * 2.2);
    // how long after alice's intervals bob's start, by the positions each last published (within
    // about 40 ms)
    const int a1 = atoi(a.status()["pos"].c_str()), bp = atoi(b.status()["pos"].c_str()), a2 = atoi(a.status()["pos"].c_str());
    const double bAfterMs = fmod((a2 >= a1 ? (a1 + a2) / 2.0 : a1) - bp + 2.0 * ilen, (double)ilen) * 1000.0 / a.m_srate;
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
    // bob alone on alice's output Player 1, and nobody on the others
    const std::vector<float> ap = a.player1Heard();
    const double aPlayerSnr = tone_snr(ap, ap.size() > len ? ap.size() - len : 0, ap.size(), b.m_freq, a.m_srate);
    const float aOthersPeak = a.otherPlayersPeak();
    const bool gotChat = contains(b.command("chatlog"), "hello from alice");
    printf("users (alice): %s", a.command("users").c_str());
    printf("users (bob): %s", b.command("users").c_str());
    auto shown = [](const std::string &users) {
      const size_t at = users.find("shown=");
      return at == std::string::npos ? std::string("-") : users.substr(at + 6, users.find('\n', at) - at - 6);
    };
    auto sa = a.status(), sb = b.status();
    printf("PHASE two END a_hears_snr=%.1f b_hears_snr=%.1f b_host_refused=%d b_got_chat=%d a_users=%s b_users=%s a_name=%s b_name=%s "
           "a_sees=%s b_sees=%s a_state=%s b_state=%s a_link=%s b_link=%s b_after_ms=%.0f a_player1_snr=%.1f a_other_players_peak=%.4f\n",
           aSnr, bSnr, refused ? 1 : 0, gotChat ? 1 : 0, sa["users"].c_str(), sb["users"].c_str(), sa["user"].c_str(), sb["user"].c_str(),
           shown(a.command("users")).c_str(), shown(b.command("users")).c_str(), sa["state"].c_str(), sb["state"].c_str(),
           sa["link"].c_str(), sb["link"].c_str(), bAfterMs, aPlayerSnr, aOthersPeak);

    // bob changes the room's bitrate, then its codec; alice's changes too, her own setting doesn't
    const std::string startKbps = a.status()["roomkbps"];
    b.command("bitrate 256");
    a.runUntil(nullptr, 10.0, [&]() { return a.status()["kbps"] == "256" && b.status()["kbps"] == "256"; });
    sa = a.status();
    sb = b.status();
    const bool sawNote = contains(b.command("chatlog"), "set the room's Opus bitrate to 256 kbps");
    b.command("codec flac");
    a.runUntil(nullptr, 10.0, [&]() { auto s = a.status(); return s["codec"] == "FLAC" && s["upload"] == "FLAC"; });
    const std::string flac = a.status()["codec"] + "/" + a.status()["upload"];
    a.command("codec opus");
    a.runUntil(nullptr, 10.0, [&]() { auto s = a.status(); return s["codec"] == "Opus" && s["upload"] == "OPUS"; });
    const std::string opus = a.status()["codec"] + "/" + a.status()["upload"];
    printf("PHASE room END start_kbps=%s a_kbps=%s b_kbps=%s a_own=%s b_own=%s b_saw_note=%d flac=%s opus=%s start_bpi=%s least_bpi=%s\n",
           startKbps.c_str(), sa["kbps"].c_str(), sb["kbps"].c_str(), sa["ownkbps"].c_str(), sb["ownkbps"].c_str(), sawNote ? 1 : 0,
           flac.c_str(), opus.c_str(), startBpi.c_str(), leastBpi.c_str());

    // alice stops her server, which drops bob; now bob can host, and alice joins him
    a.command("host off");
    const bool dropped = a.runUntil(nullptr, 15.0, [&]() { return a.status()["hosting"] == "0" && b.status()["state"] != "connected"; });
    b.command("host on");
    a.runUntil(nullptr, 15.0, [&]() { auto s = b.status(); return s["state"] == "connected" && s["hosting"] == "1"; });
    const std::string port2 = b.status()["port"];
    a.command("connect localhost:" + port2);
    a.runUntil(nullptr, 15.0, [&]() {
      auto s = a.status();
      return s["state"] == "connected" && atoi(s["users"].c_str()) >= 1 && s["roomkbps"] != "0" && s["link"] == "other";
    });
    sa = a.status();
    printf("PHASE swap END b_dropped=%d b_hosting=%s port=%s a_state=%s a_users=%s a_kbps=%s a_link=%s b_link=%s\n", dropped ? 1 : 0,
           b.status()["hosting"].c_str(), port2.c_str(), sa["state"].c_str(), sa["users"].c_str(), sa["kbps"].c_str(),
           sa["link"].c_str(), b.status()["link"].c_str());

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
  // the pitch of the clicks whose first 20 ms are all there (the recording may end in one)
  const long whole = (long)x.size() - host.m_srate / 50;
  auto pitch = [&](const std::vector<long> &clicks) {
    double sum = 0;
    int n = 0;
    for (long i : clicks)
      if (i <= whole)
      {
        sum += click_hz(x, i, host.m_srate);
        n++;
      }
    return n ? sum / n : 0.0;
  };
  const double accentHz = pitch(acc), beatHz = pitch(beats);
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
    if (off < 0) off += bpi;
    if (off >= bpi / 2.0) off -= bpi;
    const double ms = off * 60000.0 / t.tempo;
    worst = std::max(worst, fabs(ms));
    char b[32];
    snprintf(b, sizeof(b), "%s%.3f", list.empty() ? "" : ",", ms);
    list += b;
  }
  if (playing) printf("accents (ms from a host beat that is a multiple of %d): %s\n", bpi, list.c_str());
  if (playing)
    printf("SYNC phase %s n=%d max_ms=%.3f gap_err=%ld fixes=%s bars=%s accent_hz=%.0f beat_hz=%.0f beats=%d latency=%s\n", name, (int)acc.size(),
           worst, gapErr, s["barfixes"].c_str(), s["bars"].c_str(), accentHz, beatHz, (int)beats.size(), s["latency"].c_str());
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

// ---------------------------------------------------------------------------
// live: Live mode between two instances, through the plug-in's own server
// ---------------------------------------------------------------------------

// What alice plays: 20 ms chirps (500 Hz to 4 kHz, a Hann window), 250 to 450 ms apart (seeded).
// A copy 0 to 240 ms after a chirp is that chirp's; further away (the intervals), the uneven gaps
// tell which chirp a copy is (see find_pattern()).
struct Bursts
{
  explicit Bursts(int srate) : m_srate(srate), m_next(srate)
  {
    const int len = srate / 50;
    chirp.resize(len);
    const double T = (double)len / srate, f0 = 500, f1 = 4000;
    for (int i = 0; i < len; i ++)
    {
      const double t = (double)i / srate;
      const double w = 0.5 - 0.5 * cos(2 * PI * i / (len - 1));
      chirp[i] = (float)(0.5 * w * sin(2 * PI * (f0 * t + (f1 - f0) * t * t / (2 * T))));
    }
  }
  // the input at pos (called with pos going up, one sample after the other)
  float at(long pos)
  {
    if (pos >= m_next)
    {
      starts.push_back(m_next);
      m_rng = m_rng * 1103515245u + 12345u;
      m_next += (long)(m_srate * (0.25 + 0.2 * ((m_rng >> 8) & 0xffff) / 65536.0));
    }
    const long k = starts.empty() ? -1 : pos - starts.back();
    return k >= 0 && k < (long)chirp.size() ? chirp[(size_t)k] : 0.0f;
  }
  std::vector<float> chirp;
  std::vector<long> starts; // where each chirp starts, in the hosts' samples (both run in step)
private:
  int m_srate;
  long m_next;
  unsigned int m_rng = 12345;
};

static const double LIVE_WINDOW_MS = 240; // a live copy is this late at most (less than the chirps' gaps)

// The best match of the chirp in x at positions from..to (normalized cross-correlation, 1 is a
// perfect copy at any level): its position in *at, and the value.
static double best_match(const std::vector<float> &x, const std::vector<float> &c, long from, long to, long *at)
{
  const long len = (long)c.size();
  from = std::max(from, 0L);
  to = std::min(to, (long)x.size() - len);
  *at = -1;
  if (from > to) return 0;
  double cc = 0;
  for (float v : c) cc += (double)v * v;
  double e = 0, best = 0;
  for (long i = 0; i < len; i ++) e += (double)x[from + i] * x[from + i];
  for (long p = from; p <= to; p ++)
  {
    if (p > from)
    {
      e += (double)x[p + len - 1] * x[p + len - 1] - (double)x[p - 1] * x[p - 1];
      if (((p - from) & 4095) == 0)
      {
        e = 0; // again, against rounding
        for (long i = 0; i < len; i ++) e += (double)x[p + i] * x[p + i];
      }
    }
    if (e < 1e-10) continue;
    double dot = 0;
    for (long i = 0; i < len; i ++) dot += (double)x[p + i] * c[i];
    const double r = dot / sqrt(cc * e);
    if (r > best)
    {
      best = r;
      *at = p;
    }
  }
  return best;
}

struct Found
{
  long start;   // the chirp's start in alice's input
  double lagMs; // where it was found, after that (-1: not found)
  double corr;
};

static const double LIVE_MATCH = 0.5; // a normalized correlation above this is the chirp

static int found_count(const std::vector<Found> &f)
{
  int n = 0;
  for (auto &x : f) if (x.lagMs >= 0) n++;
  return n;
}

// Each chirp whose copy, from minMs to maxMs after it, lies in x (recorded from heardFrom on).
static std::vector<Found> find_chirps(const std::vector<float> &x, long heardFrom, const Bursts &b, int srate, double minMs, double maxMs)
{
  std::vector<Found> out;
  const long len = (long)b.chirp.size();
  for (long s : b.starts)
  {
    const long from = s + (long)(minMs * srate / 1000) - heardFrom, to = s + (long)(maxMs * srate / 1000) - heardFrom;
    if (from < 0 || to + len > (long)x.size()) continue;
    long at = -1;
    const double r = best_match(x, b.chirp, from, to, &at);
    Found f;
    f.start = s;
    f.corr = r;
    f.lagMs = r >= LIVE_MATCH && at >= 0 ? (at + heardFrom - s) * 1000.0 / srate : -1;
    out.push_back(f);
  }
  return out;
}

// The copies of the chirps from minMs to maxMs after them (the intervals: a window with copies of
// other chirps in it too): the lag that most chirps have a copy at (the gaps between the chirps
// are uneven, so only the right lag fits them all), and each chirp's copy near it.
static std::vector<Found> find_pattern(const std::vector<float> &x, long heardFrom, const Bursts &b, int srate, double minMs, double maxMs)
{
  const long len = (long)b.chirp.size(), tol = srate * 3 / 1000;
  // candidates: what looks like a copy in the windows of the first three chirps in x
  std::vector<long> lags;
  int tried = 0;
  for (long s : b.starts)
  {
    const long from = s + (long)(minMs * srate / 1000) - heardFrom, to = s + (long)(maxMs * srate / 1000) - heardFrom;
    if (from < 0 || to + len > (long)x.size()) continue;
    for (long p = from; p <= to;)
    {
      long at = -1;
      const double r = best_match(x, b.chirp, p, std::min(to, p + len), &at);
      if (r >= LIVE_MATCH && at >= 0)
      {
        const long lag = at + heardFrom - s;
        bool seen = false;
        for (long l : lags) if (labs(l - lag) <= tol) seen = true;
        if (!seen) lags.push_back(lag);
        p = at + len;
      }
      else
        p += len + 1;
    }
    if (++tried == 3) break;
  }
  std::vector<Found> best;
  int bestCount = -1;
  for (long lag : lags)
  {
    std::vector<Found> out;
    for (long s : b.starts)
    {
      const long c = s + lag - heardFrom;
      if (c - tol < 0 || c + tol + len > (long)x.size()) continue;
      long at = -1;
      const double r = best_match(x, b.chirp, c - tol, c + tol, &at);
      Found f;
      f.start = s;
      f.corr = r;
      f.lagMs = r >= LIVE_MATCH && at >= 0 ? (at + heardFrom - s) * 1000.0 / srate : -1;
      out.push_back(f);
    }
    const int n = found_count(out);
    if (n > bestCount)
    {
      bestCount = n;
      best = out;
    }
  }
  if (lags.empty())
    for (long s : b.starts)
    {
      const long from = s + (long)(minMs * srate / 1000) - heardFrom, to = s + (long)(maxMs * srate / 1000) - heardFrom;
      if (from >= 0 && to + len <= (long)x.size()) best.push_back(Found { s, -1, 0 });
    }
  return best;
}

// the median or another percentile of the lags found
static double lag_percentile(const std::vector<Found> &f, double pct)
{
  std::vector<double> v;
  for (auto &x : f) if (x.lagMs >= 0) v.push_back(x.lagMs);
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  const size_t i = std::min(v.size() - 1, (size_t)floor(pct / 100.0 * (v.size() - 1) + 0.5));
  return v[i];
}

// the first line of `live status` (key=value): the room's and our own
static std::map<std::string, std::string> live_room(Host &h)
{
  const std::string st = h.command("live status");
  return parse_status(st.substr(0, st.find('\n')));
}

// one line of `live status` (key=value) for the peer named `who`
static std::map<std::string, std::string> live_peer(Host &h, const std::string &who)
{
  const std::string st = h.command("live status");
  size_t at = 0;
  while (at < st.size())
  {
    size_t eol = st.find('\n', at);
    if (eol == std::string::npos) eol = st.size();
    const std::string line = st.substr(at, eol - at);
    if (line.compare(0, 5 + who.size(), "peer=" + who) == 0) return parse_status(line);
    at = eol + 1;
  }
  return std::map<std::string, std::string>();
}

// Two instances in step (one thread, as two tracks of a DAW): alice hosts the server and plays
// chirps, bob joins and plays nothing, and an NJClient without Live ("old", sending nothing)
// joins too. Before Live, bob hears alice through intervals (one interval and some late); then
// alice turns the room's Live on (!live on) and bob hears her live, the intervals' copy muted;
// on his output Player N (hers) and nowhere else; old still hears her through intervals. Then
// bob's jitter buffer goes to 40 ms and back; bob's DAW misses 32 ms once, and bob is live again
// after it; and last alice turns Live off: bob hears her intervals at once. The hosts never stop: the test
// looks at what they played while they go on. Where the test machine was busy (dropouts, blocks
// late, bob back on intervals for a while) the phase isn't judged; with too little left it is
// measured again, up to three times.
//   PORT <port>
//   LIVE ready b_offered=<0|1> b_room=<on|off> b_peers=<n> old_users=<n>
//   LIVE intervals found=<n>/<m> lag_ms=<median> old_found=<n>/<m> old_lag_ms=<median>
//   LIVE on b_live=<0|1> waited_s=<s> marker=<0|1: " · live" on alice's channel in bob's users>
//   LIVE retry <phase> clean=<n>/<m> late_blocks=<n> dropouts=<n>   (the phase again, up to 3 times)
//   LIVE phase <live|stall|buffer40> found=<n>/<of the clean> clean=<chirps judged>/<played> median_ms=<>
//        p95_ms=<> min_ms=<> max_ms=<> mean_corr=<> estimate_ms=<bob's estimate>
//        outside=<energy outside the copies / inside, where clean> interval_copy=<heard>/<checked, where clean>
//        player=<N> player_found=<n>/<m> other_players_peak=<> old_found=<n>/<m> old_lag_ms=<>
//        dropouts=<bob's for alice, in the phase> b_live=<0|1> stayed_live=<0|1: at every look, 4 a second>
//        attempts=<n> late_blocks=<blocks the test host called more than 20 ms late> late_ms=<in all>
//        late_before=<the same, before the phase> process_max_ms=<alice's longest process() call>/<bob's>
//   LIVE recovered b_live=<0|1: live and healthy for 3 s> after_s=<from the stall phase's end> fell_back=<0|1>
//        missed_ms=<bob's DAW missed>
//   LIVE off found=<n>/<m> lag_ms=<median> b_live=<0|1> b_room=<on|off>
//   chat (bob): ...
//   DONE
static int live_latency(Host &a, Host &b)
{
  const int srate = a.m_srate;
  Bursts bursts(srate);
  a.m_signal = [&bursts](long pos) { return bursts.at(pos); };
  b.m_signal = [](long) { return 0.0f; };
  a.m_noCatchUp = b.m_noCatchUp = true;
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
  if (getenv("NINJAM_E2E_LIVE_BUFFER"))
  {
    // another jitter buffer than the default, for trying things by hand
    a.command(std::string("live buffer ") + getenv("NINJAM_E2E_LIVE_BUFFER"));
    b.command(std::string("live buffer ") + getenv("NINJAM_E2E_LIVE_BUFFER"));
  }

  JNL::open_socketlib();
  NJClient old; // a client without Live
  old.config_metronome_mute = true;
  old.config_metronome = 0.0f;
  old.SetLocalChannelInfo(0, "quiet", true, 0, true, 64, true, false); // sends nothing
  old.SetLocalChannelMonitoring(0, false, 0.0f, false, 0.0f, true, true, false, false);

  // both in step: alice's block, then bob's (and old's), in real time
  auto both = [&](double seconds, bool record) {
    const long blocks = (long)ceil(seconds * srate / a.m_block);
    for (long i = 0; i < blocks; i ++)
    {
      a.block(nullptr, false);
      b.block(&old, record);
    }
  };
  auto until = [&](double timeout, std::function<bool()> pred) {
    const double end = now_seconds() + timeout;
    while (now_seconds() < end)
    {
      if (pred()) return true;
      both(0.05, false);
    }
    return pred();
  };
  // work on what was recorded (on another thread), the hosts going on meanwhile
  auto meanwhile = [&](std::function<void()> work) {
    std::atomic<bool> done(false);
    std::thread worker([&]() {
      work();
      done = true;
    });
    while (!done) both(0.02, false);
    worker.join();
  };
  int result = 0;
  auto finish = [&]() {
    old.Disconnect();
    b.command("disconnect");
    a.command("host off");
    until(10.0, [&]() { return a.status()["hosting"] == "0"; });
    JNL::close_socketlib();
  };

  if (!until(15.0, [&]() { auto s = a.status(); return s["state"] == "connected" && s["hosting"] == "1"; }))
    return fail(a, "alice did not start her server and join it");
  const std::string port = a.status()["port"];
  printf("PORT %s\n", port.c_str());
  old.Connect(("127.0.0.1:" + port).c_str(), "anonymous:old", "");
  b.command("connect localhost:" + port);
  if (!until(20.0, [&]() {
        auto sa = a.status(), sb = b.status();
        return sb["state"] == "connected" && atoi(sa["users"].c_str()) >= 2 && atoi(sb["users"].c_str()) >= 2 &&
               old.GetStatus() == NJClient::NJC_STATUS_OK && old.GetNumUsers() >= 1 && !live_peer(b, "alice@").empty();
      }))
  {
    finish();
    return fail(b, "bob and old did not join alice's server, or bob doesn't see alice as a Live player");
  }
  auto lb = live_room(b);
  printf("LIVE ready b_offered=%s b_room=%s b_peers=%s old_users=%d\n", lb["offered"].c_str(), lb["live"].c_str(), lb["peers"].c_str(),
         old.GetNumUsers());

  const int ilen = atoi(a.status()["len"].c_str());
  const double interval = (double)ilen / srate, ivMs = interval * 1000;

  // before Live: through intervals, a bit more than one interval late
  both(interval * 2.2, false);
  a.clearHeard();
  b.clearHeard();
  both(interval * 4, true);
  std::vector<Found> iv, ivOld;
  {
    const Bursts played = bursts;
    const std::vector<float> x = b.pluginHeard(), px = b.m_peerHeard;
    const long from = b.m_heardFrom;
    meanwhile([&]() {
      iv = find_pattern(x, from, played, srate, ivMs - 100, 2 * ivMs + 100);
      ivOld = find_pattern(px, from, played, srate, ivMs - 100, 2 * ivMs + 100);
    });
  }
  const double ivLag = lag_percentile(iv, 50), oldLag = lag_percentile(ivOld, 50);
  printf("LIVE intervals found=%d/%d lag_ms=%.1f old_found=%d/%d old_lag_ms=%.1f\n", found_count(iv), (int)iv.size(), ivLag,
         found_count(ivOld), (int)ivOld.size(), oldLag);

  // alice turns the room's Live on; bob hears her live from one of his intervals' start on
  a.command("live on");
  const double t0 = now_seconds();
  double lastDebug = t0;
  const bool on = until(20.0, [&]() {
    if (getenv("NINJAM_E2E_DEBUG") && now_seconds() - lastDebug >= 1.0)
    {
      lastDebug = now_seconds();
      auto pa = live_peer(a, "bob@"), pb = live_peer(b, "alice@");
      printf("debug: %.1f s: bob hears alice: healthy=%s dropouts=%s (dry %s, over %s), alice hears bob: dropouts=%s (dry %s, over %s)\n",
             lastDebug - t0, pb["healthy"].c_str(), pb["dropouts"].c_str(), pb["underruns"].c_str(), pb["overruns"].c_str(),
             pa["dropouts"].c_str(), pa["underruns"].c_str(), pa["overruns"].c_str());
    }
    return live_peer(b, "alice@")["live"] == "1";
  });
  const bool marker = contains(b.command("users"), "main \xc2\xb7 live");
  printf("LIVE on b_live=%d waited_s=%.1f marker=%d\n", on ? 1 : 0, now_seconds() - t0, marker ? 1 : 0);
  if (!on)
  {
    printf("live status (alice): %s", a.command("live status").c_str());
    printf("live status (bob): %s", b.command("live status").c_str());
    printf("chat (bob):\n%s", b.command("chatlog").c_str());
    finish();
    return fail(b, "bob does not hear alice live");
  }

  // bob's output Player N: alice's
  int player = 0;
  {
    const std::string chat = b.command("chatlog");
    const size_t at = chat.find("alice plays alone on the output Player ");
    if (at != std::string::npos) player = atoi(chat.c_str() + at + strlen("alice plays alone on the output Player "));
  }

  // after a busy spell: as after any dropouts, bob may go back to intervals at an interval's
  // start and come back after a few good seconds, at the next one; wait for that
  auto settle = [&]() {
    const double need = 2 * interval + 4.0;
    double clean = 0;
    const double end = now_seconds() + 60.0;
    int drops = atoi(live_peer(b, "alice@")["dropouts"].c_str());
    while (now_seconds() < end)
    {
      const int s0 = a.m_skips + b.m_skips;
      both(0.25, false);
      auto lq = live_peer(b, "alice@");
      const int d = atoi(lq["dropouts"].c_str());
      clean = a.m_skips + b.m_skips != s0 || d != drops || lq["live"] != "1" ? 0 : clean + 0.25;
      drops = d;
      if (clean >= need) break;
    }
  };

  // A phase: `seconds` of what bob hears, looked at in quarters of a second. A quarter is clean
  // when bob heard alice live at both its ends, without new dropouts or late blocks in it (or
  // `during`'s doing: it runs after each quarter, with the seconds so far, and returns true when
  // it disturbs the next one); the chirps and the intervals' copies are judged where it and the
  // quarters on either side are clean. Too little clean (the test machine busy: like a DAW that
  // is late, it makes dropouts, and bob goes back to intervals for a while) and the phase is
  // measured again, up to `attempts` times.
  auto phase = [&](const char *name, double seconds, int attempts, std::function<bool(double)> during) {
    for (int attempt = 1; ; attempt ++)
    {
      const int dropoutsBefore = atoi(live_peer(b, "alice@")["dropouts"].c_str());
      const int skipsBefore = a.m_skips + b.m_skips;
      a.m_maxProcessMs = b.m_maxProcessMs = 0;
      const double skippedBefore = a.m_skippedMs + b.m_skippedMs;
      b.m_recordPlayers = true;
      a.clearHeard();
      b.clearHeard();
      const long from = b.m_pos;
      std::vector<long> qEnd;  // each quarter's end (bob's position)
      std::vector<char> qBad;  // not clean
      bool liveWas = live_peer(b, "alice@")["live"] == "1", stayed = liveWas, disturb = false;
      int dropsWas = dropoutsBefore, skipsWas = skipsBefore;
      for (double done = 0; done < seconds - 1e-9; done += 0.25)
      {
        both(std::min(0.25, seconds - done), true);
        auto lq = live_peer(b, "alice@");
        const bool liveNow = lq["live"] == "1";
        const int dropsNow = atoi(lq["dropouts"].c_str()), skipsNow = a.m_skips + b.m_skips;
        qEnd.push_back(b.m_pos);
        qBad.push_back(!liveWas || !liveNow || dropsNow != dropsWas || skipsNow != skipsWas || disturb);
        stayed = stayed && liveNow;
        liveWas = liveNow;
        dropsWas = dropsNow;
        skipsWas = skipsNow;
        disturb = during && during(done + 0.25);
      }
      b.m_recordPlayers = false;
      auto lp = live_peer(b, "alice@");
      const int skips = a.m_skips + b.m_skips - skipsBefore;
      const double skippedMs = a.m_skippedMs + b.m_skippedMs - skippedBefore;
      // clean where it and its neighbours are
      auto clean = [&](long pos) {
        size_t q = 0;
        while (q < qEnd.size() && qEnd[q] <= pos) q ++;
        if (q >= qEnd.size()) return false;
        for (size_t k = q > 0 ? q - 1 : 0; k <= std::min(q + 1, qEnd.size() - 1); k ++)
          if (qBad[k]) return false;
        return true;
      };
      const long liveAt = srate * 40 / 1000; // where a chirp's live copy is, about
      const Bursts played = bursts;
      int all = 0, cleanChirps = 0;
      for (long st : played.starts)
        if (st >= from && st + (long)(LIVE_WINDOW_MS * srate / 1000) + (long)played.chirp.size() <= b.m_pos)
        {
          all++;
          if (clean(st + liveAt)) cleanChirps++;
        }
      if (cleanChirps < 0.9 * all && attempt < attempts)
      {
        printf("LIVE retry %s clean=%d/%d late_blocks=%d dropouts=%d (the test machine was busy, or bob went back to "
               "intervals: once more)\n", name, cleanChirps, all, skips, atoi(lp["dropouts"].c_str()) - dropoutsBefore);
        fflush(stdout);
        settle();
        continue;
      }
      // the chirps whose live copy is in clean quarters
      Bursts judged = played;
      judged.starts.clear();
      for (long st : played.starts)
        if (clean(st + liveAt)) judged.starts.push_back(st);
      const std::vector<float> x = b.pluginHeard(), px = b.m_peerHeard;
      std::vector<float> players[8];
      for (int p = 0; p < 8; p ++) players[p] = b.m_playersHeard[p];
      const long heardFrom = b.m_heardFrom;
      std::vector<Found> f, fo;
      int checked = 0, heard = 0, playerFound = 0, playerChecked = 0;
      float othersPeak = 0;
      double ein = 0, eout = 0;
      meanwhile([&]() {
        f = find_chirps(x, heardFrom, judged, srate, 0, LIVE_WINDOW_MS);
        // the interval's copy, where it was before Live: not heard (where bob was live, and no
        // live copy is near)
        const double liveLag = lag_percentile(f, 50);
        if (ivLag > 0)
          for (long st : played.starts)
          {
            const long at = st + (long)(ivLag * srate / 1000);
            bool near = false;
            for (long o : played.starts)
            {
              const long liveCopy = o + (long)(liveLag * srate / 1000);
              if (labs(liveCopy - at) < srate * 60 / 1000) near = true;
            }
            long pos = at - heardFrom - srate * 30 / 1000;
            if (near || !clean(at) || pos < 0 || pos + srate * 60 / 1000 + (long)played.chirp.size() > (long)x.size()) continue;
            checked++;
            long m = -1;
            if (best_match(x, played.chirp, pos, pos + srate * 60 / 1000, &m) >= LIVE_MATCH) heard++;
          }
        // what bob plays outside the live copies (from 5 ms before to 30 ms after each), where clean
        std::vector<char> inside(x.size(), 0);
        for (auto &k : f)
        {
          if (k.lagMs < 0) continue;
          const long c = k.start + (long)(k.lagMs * srate / 1000) - heardFrom;
          for (long i = std::max(0L, c - srate / 200); i < std::min((long)x.size(), c + srate * 30 / 1000); i ++) inside[(size_t)i] = 1;
        }
        for (size_t i = (size_t)srate / 2; i + srate / 2 < x.size(); i ++)
          if (inside[i]) ein += (double)x[i] * x[i];
          else if (clean(heardFrom + (long)i)) eout += (double)x[i] * x[i];
        // alice on bob's output Player N, and on no other
        for (int p = 0; p < 8; p ++)
        {
          if (p + 1 == player)
          {
            std::vector<Found> pf = find_chirps(players[p], heardFrom, judged, srate, 0, LIVE_WINDOW_MS);
            playerFound = found_count(pf);
            playerChecked = (int)pf.size();
          }
          else
            for (float v : players[p]) othersPeak = std::max(othersPeak, fabsf(v));
        }
        fo = find_pattern(px, heardFrom, played, srate, ivMs - 100, 2 * ivMs + 100);
      });
      if (getenv("NINJAM_E2E_DEBUG"))
      {
        printf("debug: %s lags:", name);
        for (auto &k : f) printf(" %.1f", k.lagMs);
        printf("\ndebug: %s quarters:", name);
        for (char bad : qBad) printf("%c", bad ? 'x' : '.');
        printf("\n");
      }
      double corr = 0;
      for (auto &k : f) corr += k.corr;
      corr = f.empty() ? 0 : corr / f.size();
      printf("LIVE phase %s found=%d/%d clean=%d/%d median_ms=%.1f p95_ms=%.1f min_ms=%.1f max_ms=%.1f mean_corr=%.3f estimate_ms=%s "
             "outside=%.5f interval_copy=%d/%d player=%d player_found=%d/%d other_players_peak=%.5f old_found=%d/%d old_lag_ms=%.1f "
             "dropouts=%d b_live=%s stayed_live=%d attempts=%d late_blocks=%d late_ms=%.0f late_before=%d/%.0f process_max_ms=%.2f/%.2f\n",
             name, found_count(f), (int)f.size(), cleanChirps, all, lag_percentile(f, 50), lag_percentile(f, 95), lag_percentile(f, 0),
             lag_percentile(f, 100), corr, lp["delay"].c_str(), ein > 0 ? eout / ein : -1.0, heard, checked, player, playerFound,
             playerChecked, othersPeak, found_count(fo), (int)fo.size(), lag_percentile(fo, 50),
             atoi(lp["dropouts"].c_str()) - dropoutsBefore, lp["live"].c_str(), stayed ? 1 : 0, attempt, skips, skippedMs,
             skipsBefore, skippedBefore, a.m_maxProcessMs, b.m_maxProcessMs);
      fflush(stdout);
      return;
    }
  };
  phase("live", 20.0, 3, nullptr);
  printf("live status (bob): %s", b.command("live status").c_str());
  printf("users (bob): %s", b.command("users").c_str());
  b.command("chat /live");
  both(0.2, false);

  // a bigger jitter buffer: later by about as much
  b.command("live buffer 40");
  both(1.0, false);
  until(20.0, [&]() { return live_peer(b, "alice@")["live"] == "1"; });
  phase("buffer40", 10.0, 3, nullptr);
  b.command("live buffer 20");
  both(1.0, false);
  until(20.0, [&]() { return live_peer(b, "alice@")["live"] == "1"; });
  both(3.0, false);

  // bob's DAW misses 32 ms once (busy loading something, an xrun), alice's goes on: bob may
  // go back to intervals for a while (with the dropouts, and as long as his clock filter takes to
  // settle), and is live again after it. NJClient's intervals are that much later for him from
  // then on (they go on with the blocks).
  bool stalled = false;
  double missedMs = 0;
  auto count = [](const std::string &text, const std::string &what) {
    int n = 0;
    for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) n ++;
    return n;
  };
  const int fellBackBefore = count(b.command("chatlog"), "alice is back on intervals");
  phase("stall", 10.0, 1, [&](double at) {
    if (stalled || at < 2.0) return false;
    stalled = true;
    const int blocks = (int)ceil(0.032 * srate / a.m_block);
    for (int i = 0; i < blocks; i ++) a.block(nullptr, false);
    b.miss(blocks, true);
    missedMs += 1000.0 * blocks * a.m_block / srate;
    return true;
  });
  {
    // live again: live and playing well for 3 s
    const double t1 = now_seconds();
    double good = 0;
    while (good < 3.0 && now_seconds() - t1 < 40.0)
    {
      both(0.25, false);
      auto lq = live_peer(b, "alice@");
      good = lq["live"] == "1" && lq["healthy"] == "1" ? good + 0.25 : 0;
    }
    printf("LIVE recovered b_live=%d after_s=%.1f fell_back=%d missed_ms=%.1f\n", good >= 3.0 ? 1 : 0, now_seconds() - t1 - good,
           count(b.command("chatlog"), "alice is back on intervals") > fellBackBefore ? 1 : 0, missedMs);
  }

  // Live off: bob hears alice's intervals at once
  a.command("live off");
  until(5.0, [&]() { return live_room(b)["live"] == "off"; });
  a.clearHeard();
  b.clearHeard();
  both(interval * 2.5, true);
  std::vector<Found> off;
  {
    const Bursts played = bursts;
    const std::vector<float> x = b.pluginHeard();
    const long from = b.m_heardFrom;
    meanwhile([&]() { off = find_chirps(x, from, played, srate, ivLag + missedMs - 30, ivLag + missedMs + 30); });
  }
  printf("LIVE off found=%d/%d lag_ms=%.1f b_live=%s b_room=%s\n", found_count(off), (int)off.size(), lag_percentile(off, 50),
         live_peer(b, "alice@")["live"].c_str(), live_room(b)["live"].c_str());
  printf("chat (bob):\n%s", b.command("chatlog").c_str());
  printf("chat (alice):\n%s", a.command("chatlog").c_str());

  finish();
  b.unload();
  a.unload();
  printf("DONE\n");
  return result;
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

// how much later than the plug-in's guess (one block after process() is called) this host hears a block (link)
static const double LATENCY_MS = 4.0;

// The host is Live with its Link on (peer, a Link session older than the plug-in's, at 133 BPM):
// the room leads it through Link.
static int link_sync(Host &host, ableton::Link &peer)
{
  Host::Transport &t = host.m_transport;
  t.on = true;
  t.followLink = true;
  host.m_link = &peer;
  host.m_latencyUs = LATENCY_MS * 1000;
  auto len_at = [&](int bpm, int bpi) { return (int)(60.0 / bpm * bpi * host.m_srate); };
  auto len_is = [&](int bpm) { return atoi(host.status()["len"].c_str()) == len_at(bpm, 4); };
  auto peer_tempo = [&]() { return peer.captureAppSessionState().tempo(); };
  auto peer_at = [&](double bpm) { return fabs(peer_tempo() - bpm) < 0.001; };
  auto set_peer_tempo = [&](double bpm) {
    auto state = peer.captureAppSessionState();
    state.setTempo(bpm, peer.clock().micros());
    peer.commitAppSessionState(state);
  };
  // records `intervals` intervals once Link is in line with them
  auto record = [&](const char *name, int intervals) {
    host.run(nullptr, 0.1);
    if (!host.runUntil(nullptr, 20.0, [&]() { return host.status()["linkaligned"] == "1"; })) return false;
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

  // the plug-in sees the host's Link (in this process, as on a network)
  if (!host.runUntil(nullptr, 10.0, [&]() { return atoi(host.status()["peers"].c_str()) >= 1; }))
  {
    printf("LINK nopeers\n");
    host.command("host off");
    host.runUntil(nullptr, 10.0, [&]() { return host.status()["hosting"] == "0"; });
    host.unload();
    printf("DONE\n");
    return 0;
  }

  // in the room (120 BPM), the plug-in leads Link: the host takes the room's tempo, no vote for its 133
  host.runUntil(nullptr, 10.0, [&]() { return peer_at(120) && host.status()["leads"] == "1"; });
  host.run(nullptr, 3.0); // past the time a vote would take
  auto s = host.status();
  printf("LINK joined room_bpm=%s peer_tempo=%.3f tempovotes=%s leads=%s link=%s restores=%s\n", s["bpm"].c_str(), peer_tempo(),
         s["tempovotes"].c_str(), s["leads"].c_str(), s["link"].c_str(), s["restores"].c_str());

  // the room's votes go to the host through Link
  if (host.command("vote bpm 100") != "ok" || host.command("vote bpi 4") != "ok") return fail(host, "the votes failed");
  if (!host.runUntil(nullptr, 20.0, [&]() { return len_is(100) && peer_at(100); }))
    return fail(host, "the room and Link did not get to 100 BPM and 4 beats");
  s = host.status();
  printf("LINK vote room_bpm=%s peer_tempo=%.3f\n", s["bpm"].c_str(), peer_tempo());

  // from an interval before the host plays, the tape has each interval the plug-in sent
  host.m_taping = true;
  host.run(nullptr, len_at(100, 4) / (double)host.m_srate + 0.3);

  // the host plays (on Link's beat); the plug-in finds when it hears the blocks: its measure holds
  // still for 3 seconds
  t.playing = true;
  host.resetLate();
  {
    double last = -1e9, since = now_seconds();
    host.runUntil(nullptr, 40.0, [&]() {
      const double l = atof(host.status()["latency"].c_str());
      if (fabs(l - last) > 0.001)
      {
        last = l;
        since = now_seconds();
      }
      return fabs(l) > 0.5 && now_seconds() - since >= 3.0;
    });
  }
  printf("LINK timing late_min=%.3f late_mean=%.3f late_max=%.3f\n", host.m_lateMin, host.m_lateSum / std::max(1L, host.m_lateN),
         host.m_lateMax);
  if (!record("link100", 4)) return fail(host, "Link did not line up with the intervals at 100 BPM");

  // the host's tempo is dragged to 109.6 (on Link, as Live does): one vote, and the room and Link at 110
  for (double bpm : { 104.0, 106.5, 108.0, 109.6 })
  {
    set_peer_tempo(bpm);
    host.run(nullptr, 0.15);
  }
  if (!host.runUntil(nullptr, 20.0, [&]() { return len_is(110) && peer_at(110); }))
    return fail(host, "the room and Link did not follow the host's tempo to 110");
  s = host.status();
  printf("LINK follow room_bpm=%s peer_tempo=%.3f tempovotes=%s\n", s["bpm"].c_str(), peer_tempo(), s["tempovotes"].c_str());
  if (!record("link110", 3)) return fail(host, "Link did not line up with the intervals at 110 BPM");

  // the host goes to 110.3, which is the room's BPM: no vote, and Link goes back to 110
  set_peer_tempo(110.3);
  host.run(nullptr, 0.5);
  if (!host.runUntil(nullptr, 15.0, [&]() { return peer_at(110); })) return fail(host, "Link did not go back to 110 from 110.3");
  host.run(nullptr, 1.0);
  s = host.status();
  printf("LINK same peer_tempo=%.3f tempovotes=%s restores=%s\n", peer_tempo(), s["tempovotes"].c_str(), s["restores"].c_str());

  // the host stops, and its Link goes off (its tempo stays at 110); another app sets Link to 125: back to 110
  t.playing = false;
  t.followLink = false;
  t.tempo = 110;
  host.run(nullptr, 1.0);
  set_peer_tempo(125);
  host.run(nullptr, 0.2);
  if (!host.runUntil(nullptr, 15.0, [&]() { return peer_at(110); })) return fail(host, "Link did not go back to 110 from 125");
  host.run(nullptr, 1.0);
  s = host.status();
  printf("LINK foreign peer_tempo=%.3f restores=%s\n", peer_tempo(), s["restores"].c_str());

  // what the others heard: Link moved the host's bars, never the intervals
  host.m_taping = false;
  std::string oddLengths;
  const int intervals = odd_intervals(host.m_tape, host.m_srate, { len_at(100, 4), len_at(110, 4) }, oddLengths);
  const int odd = oddLengths.empty() ? 0 : (int)std::count(oddLengths.begin(), oddLengths.end(), ',') + 1;
  printf("SYNC tape intervals=%d odd=%d fixes=%s odd_lengths=%s\n", intervals, odd, host.status()["barfixes"].c_str(),
         oddLengths.empty() ? "-" : oddLengths.c_str());
  host.m_tape.clear();

  // no other Link app now: the plug-in says to turn Link on in the host, and lines its intervals up
  // with the host's bars itself
  peer.enable(false);
  host.locate(9.37);
  t.playing = true;
  if (!host.runUntil(nullptr, 20.0, [&]() { auto st = host.status(); return st["peers"] == "0" && st["bars"] == "aligned"; }))
    return fail(host, "without Link, the intervals did not line up with the host's bars");
  host.runUntil(nullptr, 10.0, [&]() { return contains(host.command("chatlog"), "Turn on Link in "); });
  s = host.status();
  printf("LINK off peers=%s leads=%s bars=%s hint=%d tip=%s\n", s["peers"].c_str(), s["leads"].c_str(), s["bars"].c_str(),
         contains(host.command("chatlog"), "Turn on Link in My VST3 HostApplication if it has Link") ? 1 : 0,
         host.command("synctip").c_str());

  // the host's Link on again: the plug-in leads it again
  t.followLink = true;
  peer.enable(true);
  host.runUntil(nullptr, 10.0, [&]() { return host.status()["leads"] == "1" && peer_at(110); });
  s = host.status();
  printf("LINK back peers=%s leads=%s peer_tempo=%.3f tempovotes=%s\n", s["peers"].c_str(), s["leads"].c_str(), peer_tempo(),
         s["tempovotes"].c_str());

  // Sync off: the plug-in leaves Link, so the host's Link sees nobody
  host.command("follow off");
  host.runUntil(nullptr, 5.0, [&]() { return peer.numPeers() == 0; });
  host.run(nullptr, 2.0);
  s = host.status();
  printf("LINK syncoff link=%s peer_peers=%d\n", s["link"].c_str(), (int)peer.numPeers());

  // Sync on again: in Link again; out of the room, out of Link (with Sync on)
  host.command("follow on");
  host.runUntil(nullptr, 10.0, [&]() { return peer.numPeers() >= 1; });
  const int peersOn = (int)peer.numPeers();
  host.command("host off");
  host.runUntil(nullptr, 10.0, [&]() { auto st = host.status(); return st["hosting"] == "0" && st["state"] != "connected"; });
  host.runUntil(nullptr, 5.0, [&]() { return peer.numPeers() == 0; });
  host.run(nullptr, 2.0);
  s = host.status();
  printf("LINK left link=%s state=%s peer_peers_on=%d peer_peers=%d\n", s["link"].c_str(), s["state"].c_str(), peersOn,
         (int)peer.numPeers());
  printf("chat:\n%s", host.command("chatlog").c_str());

  peer.enable(false);
  host.m_link = nullptr;
  host.unload();
  printf("DONE\n");
  return 0;
}

int main(int argc, char **argv)
{
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc < 2)
  {
    fprintf(stderr, "usage: %s path/to/Jamsa.vst3 [samplerate [blocksize [join host:port | two | sync | link | live]]]\n", argv[0]);
    return 2;
  }
  const int srate = argc > 2 ? atoi(argv[2]) : 48000;
  const int blockSize = argc > 3 ? atoi(argv[3]) : 512;

  Host host(srate, blockSize);
  if (argc > 4 && !strcmp(argv[4], "link"))
  {
    // the host's Link goes on first, so its session is the older one when the plug-in's joins it
    ableton::Link peer(133.0);
    peer.enable(true);
    if (!host.load(argv[1])) return 1;
    return link_sync(host, peer);
  }
  if (!host.load(argv[1])) return 1;
  if (argc > 5 && !strcmp(argv[4], "join")) return join_vorbis_room(host, argv[5]);
  if (argc > 4 && !strcmp(argv[4], "sync")) return host_sync(host);
  if (argc > 4 && !strcmp(argv[4], "live"))
  {
    Host second(srate, blockSize, PEER_FREQ);
    if (!second.loadAnother(host)) return fail(host, "could not create a second instance");
    return live_latency(host, second);
  }
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
  // the public address, from the "what's my IP" service (the test's own, JAMSA_WHATSMYIP)
  host.runUntil(nullptr, 10.0, [&]() { return host.status()["lookedup"] != "?"; });
  printf("LOOKEDUP %s\n", host.status()["lookedup"].c_str());

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
  if (host.status()["kbps"] != "510") return fail(host, "the Opus bitrate is not 510 kbps by default");
  // the phases go up from 128 kbps
  host.command("bitrate 128");
  host.runUntil(&peer, 10.0, [&]() { return host.status()["kbps"] == "128"; });

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
