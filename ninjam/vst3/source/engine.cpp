/*
    NINJAM VST3 - engine.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

#include "engine.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>

#if defined(_WIN32)
#define SECURITY_WIN32
#include <windows.h>
#include <security.h>
#include <lm.h>
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "netapi32.lib")
#else
#include <pwd.h>
#include <unistd.h>
#endif

#include "njclient.h"
#include "mpb.h"
#include "hostserver.h"
#include "liveclient.h"

namespace ninjam_vst3 {

static const size_t MAX_CHAT_LINES = 1000;
static const char *LOCAL_CHANNEL_NAME = "main";
static const double LICENSE_WAIT_SECONDS = 60.0;
static const double PUBLISH_INTERVAL = 0.03;
static const double SYNC_NOTE_DELAY = 3.0; // the host's tempo differs this long before the note shows
static const double LINK_POLL = 0.05;
static const double LINK_DISCOVERY = 2.0;  // Link finds the other apps on the network within this, once on
static const double LINK_HINT_DELAY = 3.0; // in a room, with no other Link app for this long: "Turn on Link in Live"
static const double LIVE_POLL = 0.03;      // the Live client's events and statistics
static const double LIVE_NO_ANSWER = 5.0;  // the server's UDP port doesn't answer for this long: say so
static const double LIVE_RETRY = 10.0;     // the Live client couldn't start: try again after this
static const double LIVE_RECOVER = 3.0;    // back on intervals for dropouts: Live again once it plays well this long
static const double LIVE_REBUFFER = 2.5;   // a new jitter buffer restarts the streams: no switching to intervals for that

static double now_seconds()
{
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::string trim(const std::string &s)
{
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= ' ') a++;
  while (b > a && (unsigned char)s[b-1] <= ' ') b--;
  return s.substr(a, b - a);
}

static std::string fourcc_text(unsigned int f)
{
  std::string s;
  for (int i = 0; i < 4; i ++) s += (char)((f >> (8*i)) & 0xff);
  return s;
}

static std::string codec_name(int mask)
{
  if (mask & CLIENT_CAP_CODEC_FLAC) return "FLAC";
  if (mask & CLIENT_CAP_CODEC_OPUS) return "Opus";
  return "Vorbis";
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

static const char *STATE_HEADER = "NINJAM-VST3-STATE 1";

// /help, and after an unknown /command
static const char *HELP_TEXT = "Commands: /me, /msg name text, /topic, /bpm, /bpi, /kick, /admin, /live (how the Live players sound), /live buffer 30 (Live's jitter buffer in ms); !vote bpm 120, !vote bpi 16, !codec opus|flac, !bitrate 128 and !live on|off (Live mode: the room's Jamsa players hear each other in real time) go to the server as typed.";

static std::string escape(const std::string &s)
{
  std::string o;
  for (char c : s)
  {
    if (c == '\\') o += "\\\\";
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else o += c;
  }
  return o;
}

static std::string unescape(const std::string &s)
{
  std::string o;
  for (size_t i = 0; i < s.size(); i ++)
  {
    if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
    const char c = s[++i];
    o += c == 'n' ? '\n' : c == 'r' ? '\r' : c == 't' ? '\t' : c;
  }
  return o;
}

// numbers are stored as millionths, so the state reads the same in every locale
static std::string num(float v)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%ld", (long)lround(v * 1000000.0));
  return buf;
}

static bool read_num(const std::string &s, float *out)
{
  if (s.empty()) return false;
  char *end = nullptr;
  const long v = strtol(s.c_str(), &end, 10);
  if (!end || *end) return false;
  *out = (float)(v / 1000000.0);
  return true;
}

static float clamp_volume(float v) { return v != v ? 1.0f : std::min(std::max(v, 0.0f), 4.0f); }
static float clamp_pan(float v) { return v != v ? 0.0f : std::min(std::max(v, -1.0f), 1.0f); }

constexpr int Settings::OPUS_KBPS[];
constexpr int Settings::MIN_LIVE_BUFFER_MS, Settings::MAX_LIVE_BUFFER_MS, Settings::DEFAULT_LIVE_BUFFER_MS;

int Settings::nearestOpusKbps(int kbps)
{
  int best = DEFAULT_OPUS_KBPS;
  for (int k : OPUS_KBPS)
    if (abs(k - kbps) < abs(best - kbps)) best = k;
  return best;
}

std::string Settings::serialize() const
{
  std::string s = std::string(STATE_HEADER) + "\n";
  s += "user=" + escape(userName) + "\n";
  s += "server=" + escape(server) + "\n";
  s += "input_volume=" + num(inputVolume) + "\n";
  s += "metronome_volume=" + num(metronomeVolume) + "\n";
  s += std::string("metronome_mute=") + (metronomeMute ? "1" : "0") + "\n";
  s += std::string("monitor=") + (monitor ? "1" : "0") + "\n";
  s += std::string("transmit=") + (transmit ? "1" : "0") + "\n";
  s += "opus_kbps=" + std::to_string(opusKbps) + "\n";
  s += std::string("follow_host_tempo=") + (followHostTempo ? "1" : "0") + "\n";
  s += "live_buffer_ms=" + std::to_string(liveBufferMs) + "\n";
  if (editorWidth > 0 && editorHeight > 0) s += "editor_window=" + std::to_string(editorWidth) + "x" + std::to_string(editorHeight) + "\n";
  for (auto &r : remote)
    s += "remote=" + escape(r.first) + "\t" + num(r.second.volume) + "\t" + num(r.second.pan) + "\t" + (r.second.mute ? "1" : "0") + "\n";
  return s;
}

bool Settings::deserialize(const std::string &data)
{
  if (data.compare(0, strlen(STATE_HEADER), STATE_HEADER) != 0) return false;
  Settings s;
  size_t pos = 0;
  while (pos < data.size())
  {
    size_t eol = data.find('\n', pos);
    if (eol == std::string::npos) eol = data.size();
    const std::string line = data.substr(pos, eol - pos);
    pos = eol + 1;
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
    float v;
    if (key == "user") s.userName = unescape(value);
    else if (key == "server") s.server = unescape(value);
    else if (key == "input_volume" && read_num(value, &v)) s.inputVolume = clamp_volume(v);
    else if (key == "metronome_volume" && read_num(value, &v)) s.metronomeVolume = clamp_volume(v);
    else if (key == "metronome_mute") s.metronomeMute = value == "1";
    else if (key == "monitor") s.monitor = value == "1";
    else if (key == "transmit") s.transmit = value == "1";
    else if (key == "follow_host_tempo") s.followHostTempo = value == "1";
    else if (key == "opus_kbps")
    {
      char *end = nullptr;
      const long k = strtol(value.c_str(), &end, 10);
      if (end && !*end && !value.empty()) s.opusKbps = nearestOpusKbps((int)std::min(std::max(k, 0L), 100000L));
    }
    else if (key == "live_buffer_ms")
    {
      char *end = nullptr;
      const long ms = strtol(value.c_str(), &end, 10);
      if (end && !*end && !value.empty())
        s.liveBufferMs = (int)std::min(std::max(ms, (long)MIN_LIVE_BUFFER_MS), (long)MAX_LIVE_BUFFER_MS);
    }
    // "editor_size" was the size of the editor before its 720 x 330 layout: not used
    else if (key == "editor_window")
    {
      int w = 0, h = 0;
      if (sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w < 10000 && h < 10000)
      {
        s.editorWidth = w;
        s.editorHeight = h;
      }
    }
    else if (key == "remote")
    {
      // name \t volume \t pan \t mute (the name is escaped, so it holds no tab)
      std::vector<std::string> f;
      size_t a = 0;
      for (;;)
      {
        const size_t t = value.find('\t', a);
        f.push_back(value.substr(a, t == std::string::npos ? std::string::npos : t - a));
        if (t == std::string::npos) break;
        a = t + 1;
      }
      if (f.size() < 4) continue;
      Remote r;
      float vol, pan;
      if (!read_num(f[1], &vol) || !read_num(f[2], &pan)) continue;
      r.volume = clamp_volume(vol);
      r.pan = clamp_pan(pan);
      r.mute = f[3] == "1";
      if (s.remote.size() < 500) s.remote[unescape(f[0])] = r;
    }
  }
  *this = s;
  return true;
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

std::string Engine::displayName(const std::string &user)
{
  const size_t at = user.find('@');
  if (at == std::string::npos) return user;
  // A server that lets one name in more than once ("AnonymousUsers multi") calls the second
  // "name@1.2.3.4.2": keep the ".2", so the two can be told apart.
  const size_t colon = user.rfind(':'), from = colon != std::string::npos && colon > at ? colon + 1 : at + 1;
  const size_t dot = user.rfind('.');
  if (std::count(user.begin() + from, user.end(), '.') == 4 && dot + 1 < user.size() && dot + 4 > user.size() &&
      std::all_of(user.begin() + dot + 1, user.end(), [](char c) { return c >= '0' && c <= '9'; }))
    return user.substr(0, at) + user.substr(dot);
  return user.substr(0, at);
}

std::string Engine::firstName(const std::string &fullName)
{
  std::string n = trim(fullName);
  n = n.substr(0, n.find_first_of(" \t,@")); // a full name's first word; a login that is an email address, its name
  if (n.size() > 32) n.resize(32);
  return n;
}

#if defined(_WIN32)
static std::string utf8(const wchar_t *w)
{
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (n <= 1) return "";
  std::string s((size_t)n - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
  return s;
}
#endif

std::string Engine::defaultUserName()
{
  std::string full, login;
#if defined(_WIN32)
  // a domain account's display name, else a local or Microsoft account's full name
  wchar_t buf[256];
  ULONG n = 256;
  if (GetUserNameExW(NameDisplay, buf, &n)) full = utf8(buf);
  wchar_t name[257];
  DWORD ln = 257;
  if (GetUserNameW(name, &ln))
  {
    login = utf8(name);
    USER_INFO_10 *info = nullptr;
    if (trim(full).empty() && NetUserGetInfo(nullptr, name, 10, (LPBYTE *)&info) == NERR_Success && info)
    {
      if (info->usri10_full_name) full = utf8(info->usri10_full_name);
      NetApiBufferFree(info);
    }
  }
#else
  // the account's full name is the first field of its GECOS ("Leo Stern,,,")
  struct passwd pw, *found = nullptr;
  char buf[4096];
  if (getpwuid_r(getuid(), &pw, buf, sizeof(buf), &found) == 0 && found)
  {
    if (pw.pw_gecos) full = std::string(pw.pw_gecos).substr(0, std::string(pw.pw_gecos).find(','));
    if (pw.pw_name) login = pw.pw_name;
  }
#endif
  const std::string first = firstName(full);
  return first.empty() ? firstName(login) : first;
}

std::string Engine::remoteKey(const std::string &user, int channel)
{
  std::string n = displayName(user);
  for (auto &c : n) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return n + "/" + std::to_string(channel);
}

Engine::Engine()
{
  m_settings.userName = defaultUserName();
  m_link.reset(new LinkSync);
  m_client.reset(new NJClient);
  m_client->LicenseAgreementCallback = licenseCallback;
  m_client->LicenseAgreement_User = this;
  m_client->ChatMessage_Callback = chatCallback;
  m_client->ChatMessage_User = this;
  // Live mode: the server tells us about it, and NJClient plays a player's Live slot
  // (liveAudio()) instead of their intervals while we ask it to
  m_live.reset(new jamsa_live::Client);
  m_client->config_live = true;
  m_client->LiveAudio_Callback = liveAudio;
  m_client->LiveAudio_User = this;

  // our input, in stereo (source channels 0 and 1)
  m_client->SetLocalChannelInfo(0, LOCAL_CHANNEL_NAME, true, 0 | 1024, true, m_settings.opusKbps, true, m_settings.transmit);
  // each player (all their channels) on an output pair of their own, after the main pair: the
  // least used one when they join
  m_client->config_remote_autochan = 2;
  m_client->config_remote_autochan_nch = 2 + 2 * NUM_PLAYER_OUTS;
  applyLocalSettings();

  setupProcessing(48000, 1024);
}

Engine::~Engine()
{
  stop();
}

void Engine::start()
{
  if (m_running) return;
  JNL::open_socketlib();
  m_quit = false;
  m_running = true;
  m_thread = std::thread([this]() { threadProc(); });
}

void Engine::stop()
{
  if (!m_running) return;
  m_quit = true;
  m_licenseAnswer = -1;
  m_cmdCv.notify_all();
  if (m_thread.joinable()) m_thread.join();
  m_running = false;

  // what the network thread would have done on its way out
  {
    std::lock_guard<std::mutex> audio(m_audioMutex);
    m_link->release();
  }
  m_link->use(false);
  m_linkLeads = false;
  clientDisconnect();
  if (m_hosting) HostServer::instance().stop(this);
  m_hosting = false;
  JNL::close_socketlib();
}

// ---------------------------------------------------------------------------
// audio thread
// ---------------------------------------------------------------------------

void Engine::setupProcessing(double sampleRate, int maxBlockSize)
{
  m_sampleRate = std::max(1, (int)lround(sampleRate));
  m_maxBlock = std::max(32, std::min(maxBlockSize, 1 << 16));
  m_bufL.assign(m_maxBlock, 0.0f);
  m_bufR.assign(m_maxBlock, 0.0f);
  m_spare.assign(m_maxBlock, 0.0f);
  m_monL.assign(m_maxBlock, 0.0f);
  m_monR.assign(m_maxBlock, 0.0f);
  m_mix.assign(2 + 2 * NUM_PLAYER_OUTS, std::vector<float>(m_maxBlock, 0.0f));
  m_liveL.assign(m_maxBlock, 0.0f);
  m_liveR.assign(m_maxBlock, 0.0f);
  m_live->prepare(m_sampleRate, m_maxBlock);
  m_barSync.reset();
  m_click.setSampleRate(m_sampleRate);
  m_sampleTime = 0;
  m_link->resetTiming();
}

void Engine::process(float **in, int numIn, float **out, int numOut, int frames, const HostTiming *timing,
                     const PlayerOuts *playerOuts)
{
  if (frames <= 0 || numOut < 1 || !out || !out[0]) return;
  auto silencePlayerOuts = [&]() {
    if (playerOuts)
      for (int b = 0; b < NUM_PLAYER_OUTS; b ++)
        for (int c = 0; c < playerOuts->channels[b]; c ++)
          if (playerOuts->bus[b] && playerOuts->bus[b][c]) memset(playerOuts->bus[b][c], 0, sizeof(float) * frames);
  };
  m_processCalls.fetch_add(1, std::memory_order_relaxed);
  const float gain = m_inputGain.load(std::memory_order_relaxed);
  const int srate = m_sampleRate.load(std::memory_order_relaxed);
  float *inL = numIn > 0 && in ? in[0] : nullptr;
  float *inR = numIn > 1 && in ? in[1] : inL;
  m_hostTempo.store(timing && timing->position.tempo > 0 ? timing->position.tempo : 0.0, std::memory_order_relaxed);
  const int64_t sampleTime = m_sampleTime;
  m_sampleTime += frames;

  std::unique_lock<std::mutex> audio(m_audioMutex, std::try_to_lock);
  if (!audio.owns_lock())
  {
    // the network thread is connecting or disconnecting: a moment of silence
    for (int c = 0; c < numOut; c ++)
      if (out[c]) memset(out[c], 0, sizeof(float) * frames);
    silencePlayerOuts();
    return;
  }

  // Ableton Link, in the instance that leads it: the room's tempo, and the intervals' beat
  bool linkLeads = false;
  if (!(timing && timing->offline))
  {
    LinkSync::Block block;
    block.sampleTime = sampleTime;
    block.frames = frames;
    block.sampleRate = srate;
    LinkSync::Interval iv;
    int length = 0;
    iv.running = m_client->IsIntervalRunning();
    m_client->GetPosition(&iv.pos, &length);
    iv.len = m_client->GetUsualIntervalLength();
    iv.bpi = m_client->GetBPI();
    iv.bpm = m_client->GetActualBPM();
    LinkSync::Host host;
    if (timing)
    {
      host.playing = timing->position.playing;
      host.hasPosition = timing->position.hasPosition;
      host.ppq = timing->position.ppq;
      host.tempo = timing->position.tempo;
    }
    linkLeads = m_link->process(block, iv, host);
  }
  m_linkLeads.store(linkLeads, std::memory_order_relaxed);

  // a stopped host: we send silence (and the network thread stops sending at the next interval)
  const bool stopped = timing && timing->transport && !timing->offline && !timing->position.playing;
  m_hostStopped.store(stopped, std::memory_order_relaxed);
  const bool offline = timing && timing->offline;
  m_offline.store(offline, std::memory_order_relaxed);
  if (!offline) m_hostFrames.store(frames, std::memory_order_relaxed);
  // Live: not while rendering faster than real time (NJClient plays the intervals then)
  const bool live = !offline && m_liveOn.load(std::memory_order_relaxed);
  const bool liveSend = m_transmit.load(std::memory_order_relaxed) && !stopped;
  int liveEnd = 0;
  m_liveBlock = false;
  const bool monitor = m_monitor.load(std::memory_order_relaxed) && !m_anySolo.load(std::memory_order_relaxed);

  // the host's bars: the interval playing now may end a little early or late, so the next
  // ones start on them. Not while Link leads the host: its bars follow the intervals then.
  if (m_barReset.exchange(false, std::memory_order_acquire)) m_barSync.reset();
  if (!m_follow.load(std::memory_order_relaxed) || !timing || linkLeads) m_barSync.reset();
  else if (!timing->offline)
  {
    BarSync::Interval iv;
    int length = 0;
    iv.running = m_client->IsIntervalRunning();
    m_client->GetPosition(&iv.pos, &length);
    iv.len = m_client->GetUsualIntervalLength();
    iv.bpi = m_client->GetBPI();
    iv.bpm = m_client->GetActualBPM();
    iv.count = m_client->GetLoopCount();
    const int end = m_barSync.process(timing->position, frames, srate, iv);
    if (end > 0) m_client->SetIntervalEnd(end);
  }
  // where the next interval starts in the host, while they are (or get) in line: through BarSync,
  // or through Link while the host plays at the room's tempo
  double nextPpq = -1;
  const bool linkInLine = linkLeads && timing && timing->position.playing && timing->position.hasPosition &&
                          fabs(timing->position.tempo - m_client->GetActualBPM()) <= BarSync::SAME_TEMPO;
  if (timing && (m_barSync.state() == BarSync::Aligned || m_barSync.state() == BarSync::Lining || linkInLine) && srate > 0)
  {
    const HostPosition &p = timing->position;
    int pos = 0, length = 0;
    m_client->GetPosition(&pos, &length);
    nextPpq = p.ppq + (length - pos) / (double)srate * p.tempo / 60.0;
    if (p.cycleActive && p.cycleEnd > p.cycleStart && p.ppq < p.cycleEnd && nextPpq >= p.cycleEnd)
      nextPpq = p.cycleStart + fmod(nextPpq - p.cycleStart, p.cycleEnd - p.cycleStart);
  }
  m_nextIntervalPpq.store(nextPpq, std::memory_order_relaxed);
  if (timing) m_timeSig.store(timing->position.timeSigNum > 0 && timing->position.timeSigDen > 0
                              ? timing->position.timeSigNum * 256 + timing->position.timeSigDen : 0, std::memory_order_relaxed);
  m_barState.store(m_barSync.state(), std::memory_order_relaxed);
  m_barErrorMs.store(m_barSync.errorMs(), std::memory_order_relaxed);
  m_barFixes.store(m_barSync.corrections(), std::memory_order_relaxed);

  // NJClient clears its output before it reads its input, so it gets a copy of the input
  const float click = m_clickVolume.load(std::memory_order_relaxed);
  for (int done = 0; done < frames; )
  {
    if (live && done == liveEnd)
    {
      // Live, once per host block (a block longer than the host said it would be, in pieces):
      // our input goes out, and each slot's audio for these samples comes in
      m_liveStart = done;
      liveEnd = std::min(frames, done + m_maxBlock);
      const int len = liveEnd - done;
      for (int i = 0; i < len; i ++)
      {
        m_liveL[i] = inL ? inL[done + i] * gain : 0.0f;
        m_liveR[i] = inR ? inR[done + i] * gain : 0.0f;
      }
      m_live->processBlock(m_liveL.data(), m_liveR.data(), len, liveSend);
      m_liveBlock = true;
    }
    int n = std::min(frames - done, m_maxBlock);
    if (live) n = std::min(n, liveEnd - done);
    // a piece doesn't go over the end of the interval playing, so the click knows which
    // interval (and tempo) each sample is in
    int pos = 0, length = 0;
    m_client->GetPosition(&pos, &length);
    if (m_client->IsIntervalRunning() && pos < length) n = std::min(n, length - pos);
    for (int i = 0; i < n; i ++)
    {
      m_monL[i] = inL ? inL[done + i] * gain : 0.0f;
      m_monR[i] = inR ? inR[done + i] * gain : 0.0f;
      m_bufL[i] = stopped ? 0.0f : m_monL[i];
      m_bufR[i] = stopped ? 0.0f : m_monR[i];
    }
    float *ins[2] = { m_bufL.data(), m_bufR.data() };
    float *mix[2 + 2 * NUM_PLAYER_OUTS];
    for (int c = 0; c < 2 + 2 * NUM_PLAYER_OUTS; c ++) mix[c] = m_mix[c].data();
    m_pieceStart = done; // for liveAudio()
    m_client->AudioProc(ins, 2, mix, 2 + 2 * NUM_PLAYER_OUTS, n, srate);

    // the main output: everyone, and your monitoring (NJClient's own is muted, so it goes on
    // while nothing is sent); each player output: that player alone
    float *outs[2] = { out[0] + done, numOut > 1 && out[1] ? out[1] + done : m_spare.data() };
    for (int i = 0; i < n; i ++)
    {
      float l = mix[0][i], r = mix[1][i];
      for (int p = 0; p < NUM_PLAYER_OUTS; p ++)
      {
        l += mix[2 + 2 * p][i];
        r += mix[3 + 2 * p][i];
      }
      if (monitor)
      {
        l += m_monL[i];
        r += m_monR[i];
      }
      outs[0][i] = l;
      outs[1][i] = r;
    }
    if (playerOuts)
      for (int p = 0; p < NUM_PLAYER_OUTS; p ++)
      {
        float **bus = playerOuts->bus[p];
        if (!bus) continue;
        for (int c = 0; c < playerOuts->channels[p]; c ++)
          if (bus[c]) memcpy(bus[c] + done, mix[2 + 2 * p + (c & 1)], sizeof(float) * n);
      }

    // the metronome, on the beats of the interval these samples played in
    if (m_client->IsIntervalRunning())
    {
      m_client->GetPosition(&pos, &length);
      m_click.render(outs[0], numOut > 1 && out[1] ? outs[1] : nullptr, n, pos - n, m_client->GetUsualIntervalLength(),
                     m_client->GetBPI(), click);
    }
    else m_click.reset();
    done += n;
  }
  m_liveBlock = false;
}

// NJClient asks for a Live slot's audio (in AudioProc(), on the audio thread)
bool Engine::liveAudio(void *user, int slot, int offset, int len, const float **left, const float **right)
{
  Engine *_this = (Engine *)user;
  if (!_this->m_liveBlock || !_this->m_liveOn.load(std::memory_order_relaxed) || slot < 0 ||
      slot >= jamsa_live::Client::MAX_PEERS)
    return false; // the room turned Live off, or no Live this block: the intervals play
  const int at = _this->m_pieceStart - _this->m_liveStart + offset;
  if (at < 0 || at + len > _this->m_maxBlock) return false;
  *left = _this->m_live->stem(slot, 0) + at;
  *right = _this->m_live->stem(slot, 1) + at;
  return true;
}

// ---------------------------------------------------------------------------
// any thread
// ---------------------------------------------------------------------------

void Engine::getSnapshot(Snapshot &out) const
{
  std::lock_guard<std::mutex> lock(m_snapMutex);
  out = m_snap;
}

unsigned long long Engine::getChat(std::vector<ChatLine> &out, size_t maxLines) const
{
  std::lock_guard<std::mutex> lock(m_chatMutex);
  const size_t n = std::min(maxLines, m_chat.size());
  out.assign(m_chat.end() - n, m_chat.end());
  return m_chatRevision;
}

Settings Engine::getSettings() const
{
  std::lock_guard<std::mutex> lock(m_settingsMutex);
  return m_settings;
}

void Engine::setSettings(const Settings &s)
{
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    const std::string name = m_settings.userName;
    m_settings = s;
    if (m_settings.userName.empty()) m_settings.userName = name; // a set saved before you typed one keeps the default
    m_settingsRevision ++;
  }
  m_inputGain = s.inputVolume;
  if (m_follow.exchange(s.followHostTempo) != s.followHostTempo) m_barReset = true;
  post([this]() {
    applyLocalSettings();
    m_appliedRemote.clear(); // channels already here get the loaded settings too
  });
}

void Engine::post(std::function<void()> fn)
{
  {
    std::lock_guard<std::mutex> lock(m_cmdMutex);
    m_commands.push_back(std::move(fn));
  }
  m_cmdCv.notify_all();
}

void Engine::addChat(ChatLine::Kind kind, const std::string &who, const std::string &text)
{
  std::lock_guard<std::mutex> lock(m_chatMutex);
  ChatLine l;
  l.kind = kind;
  l.who = who;
  l.text = text;
  m_chat.push_back(l);
  while (m_chat.size() > MAX_CHAT_LINES) m_chat.pop_front();
  m_chatRevision ++;
}

void Engine::connect(const std::string &server, const std::string &userName)
{
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    m_settings.server = trim(server);
    m_settings.userName = trim(userName);
  }
  const std::string s = trim(server), u = trim(userName);
  post([this, s, u]() { doConnect(s, u); });
}

void Engine::disconnect()
{
  if (m_licenseWaiting) m_licenseAnswer = -1; // the network thread is waiting for an answer
  post([this]() { doDisconnect(nullptr); });
}

void Engine::vote(const std::string &what, int value)
{
  sendChat("!vote " + what + " " + std::to_string(value));
}

void Engine::sendChat(const std::string &text)
{
  const std::string t = trim(text);
  if (t.empty()) return;
  post([this, t]() {
    auto starts = [&t](const char *p) {
      const size_t n = strlen(p);
      if (t.size() < n) return false;
      for (size_t i = 0; i < n; i ++)
        if (tolower((unsigned char)t[i]) != p[i]) return false;
      return true;
    };
    // ours, not the server's: also out of a room
    if ((t.size() == 5 && starts("/live")) || starts("/live "))
    {
      liveChatCommand(trim(t.substr(5)));
      return;
    }
    if ((t.size() == 5 && starts("/help")) || starts("/help "))
    {
      addChat(ChatLine::System, "", HELP_TEXT);
      return;
    }
    if (m_state != ConnState::Connected)
    {
      addChat(ChatLine::Error, "", "Not connected, so nobody can read that.");
      return;
    }
    if (t[0] != '/' || starts("/me "))
    {
      m_client->ChatMessage_Send("MSG", t.c_str());
    }
    else if (starts("/topic ") || starts("/kick ") || starts("/bpm ") || starts("/bpi "))
    {
      m_client->ChatMessage_Send("ADMIN", t.c_str() + 1);
    }
    else if (starts("/admin "))
    {
      m_client->ChatMessage_Send("ADMIN", trim(t.substr(7)).c_str());
    }
    else if (starts("/msg "))
    {
      const std::string rest = trim(t.substr(5));
      const size_t sp = rest.find(' ');
      const std::string who = rest.substr(0, sp), what = sp == std::string::npos ? "" : trim(rest.substr(sp));
      if (who.empty() || what.empty()) addChat(ChatLine::Error, "", "/msg needs a name and a message.");
      else
      {
        m_client->ChatMessage_Send("PRIVMSG", who.c_str(), what.c_str());
        addChat(ChatLine::Private, "-> " + who, what);
      }
    }
    else addChat(ChatLine::Error, "", std::string("Unknown command. ") + HELP_TEXT);
  });
}

void Engine::answerLicense(bool accept)
{
  m_licenseAnswer = accept ? 1 : -1;
  m_cmdCv.notify_all();
}

void Engine::setHosting(bool on)
{
  post([this, on]() { doSetHosting(on); });
}

void Engine::setUserName(const std::string &name)
{
  std::lock_guard<std::mutex> lock(m_settingsMutex);
  m_settings.userName = trim(name);
}

void Engine::setServerText(const std::string &server)
{
  std::lock_guard<std::mutex> lock(m_settingsMutex);
  m_settings.server = trim(server);
}

void Engine::setInputVolume(float v)
{
  v = clamp_volume(v);
  m_inputGain = v;
  std::lock_guard<std::mutex> lock(m_settingsMutex);
  m_settings.inputVolume = v;
}

void Engine::setTransmit(bool on)
{
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    m_settings.transmit = on;
  }
  post([this]() { applyLocalSettings(); });
}

void Engine::setMonitor(bool on)
{
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    m_settings.monitor = on;
  }
  post([this]() { applyLocalSettings(); });
}

void Engine::setMetronome(float volume, bool mute)
{
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    m_settings.metronomeVolume = clamp_volume(volume);
    m_settings.metronomeMute = mute;
  }
  post([this]() { applyLocalSettings(); });
}

void Engine::setOpusKbps(int kbps)
{
  const int k = Settings::nearestOpusKbps(kbps);
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    m_settings.opusKbps = k;
  }
  post([this, k]() {
    // a server that sets the room's bitrate: the room's changes, for everyone
    const int room = m_state == ConnState::Connected ? m_client->GetRoomOpusKbps() : 0;
    if (room > 0 && room != k) m_client->ChatMessage_Send("MSG", ("!bitrate " + std::to_string(k)).c_str());
    applyLocalSettings();
  });
}

void Engine::setRoomLive(bool on)
{
  post([this, on]() {
    if (m_state == ConnState::Connected) m_client->ChatMessage_Send("MSG", on ? "!live on" : "!live off");
    else addChat(ChatLine::Error, "", "Not connected: Live mode is the room's, so join a room first.");
  });
}

void Engine::setLiveBufferMs(int ms)
{
  ms = std::min(std::max(ms, Settings::MIN_LIVE_BUFFER_MS), Settings::MAX_LIVE_BUFFER_MS);
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    if (m_settings.liveBufferMs == ms) return;
    m_settings.liveBufferMs = ms;
  }
  post([this]() { applyLocalSettings(); });
}

void Engine::setRoomCodec(const std::string &codec)
{
  if (codec != "opus" && codec != "flac") return;
  post([this, codec]() {
    if (m_state == ConnState::Connected) m_client->ChatMessage_Send("MSG", ("!codec " + codec).c_str());
  });
}

int Engine::sendKbps()
{
  const int room = m_state == ConnState::Connected ? m_client->GetRoomOpusKbps() : 0;
  return room > 0 ? room : getSettings().opusKbps;
}

void Engine::applySendKbps()
{
  // NJClient starts a new encoder at the next interval when the bitrate changes
  const int want = sendKbps();
  int kbps = 0;
  m_client->GetLocalChannelInfo(0, nullptr, &kbps, nullptr);
  if (kbps != want) m_client->SetLocalChannelInfo(0, nullptr, false, 0, true, want, false, false);
}

void Engine::setEditorSize(int width, int height)
{
  std::lock_guard<std::mutex> lock(m_settingsMutex);
  m_settings.editorWidth = width;
  m_settings.editorHeight = height;
}

void Engine::setFollowHostTempo(bool on)
{
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    if (m_settings.followHostTempo == on) return;
    m_settings.followHostTempo = on;
  }
  m_follow = on;
  m_barReset = true;
  post([this, on]() {
    // a vote for the host's tempo, if the room's differs, unless Live follows the room through Link
    if (on) m_armPending = true;
    m_lastLinkPoll = 0; // Link on or off now
    updateLink();
  });
}

void Engine::setHostName(const std::string &name)
{
  std::lock_guard<std::mutex> lock(m_settingsMutex);
  m_hostName = name;
}

void Engine::setRemote(const std::string &user, int channel, RemoteField field, float value)
{
  const std::string key = remoteKey(user, channel);
  if (field != RemoteSolo)
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    Settings::Remote &r = m_settings.remote[key];
    if (field == RemoteVolume) r.volume = clamp_volume(value);
    else if (field == RemotePan) r.pan = clamp_pan(value);
    else r.mute = value > 0.5f;
  }
  {
    // the UI sees its change at once, not a frame later
    std::lock_guard<std::mutex> lock(m_snapMutex);
    for (auto &u : m_snap.users)
      if (u.name == user)
        for (auto &c : u.channels)
          if (c.index == channel)
          {
            if (field == RemoteVolume) c.volume = value;
            else if (field == RemotePan) c.pan = value;
            else if (field == RemoteMute) c.mute = value > 0.5f;
            else c.solo = value > 0.5f;
          }
  }
  post([this, user, channel, field, value]() {
    const int u = findUser(user);
    if (u < 0) return;
    m_client->SetUserChannelState(u, channel, false, false,
                                  field == RemoteVolume, clamp_volume(value),
                                  field == RemotePan, clamp_pan(value),
                                  field == RemoteMute, value > 0.5f,
                                  field == RemoteSolo, value > 0.5f);
  });
}

// ---------------------------------------------------------------------------
// network thread
// ---------------------------------------------------------------------------

void Engine::runCommands()
{
  for (;;)
  {
    std::function<void()> fn;
    {
      std::lock_guard<std::mutex> lock(m_cmdMutex);
      if (m_commands.empty()) return;
      fn = std::move(m_commands.front());
      m_commands.pop_front();
    }
    fn();
  }
}

void Engine::threadProc()
{
  while (!m_quit)
  {
    runCommands();

    for (int i = 0; i < 100 && !m_quit; i ++)
      if (m_client->Run()) break; // nonzero: nothing more to do for now

    updateConnection();
    applySendKbps(); // the room's bitrate may have changed
    applyBroadcast(); // the host may have started or stopped
    updateLive();
    updateLink();
    followHostTempo();

    const double t = now_seconds();
    if (t - m_lastPublish >= PUBLISH_INTERVAL)
    {
      m_lastPublish = t;
      publish();
    }

    std::unique_lock<std::mutex> lock(m_cmdMutex);
    if (m_commands.empty() && !m_quit) m_cmdCv.wait_for(lock, std::chrono::milliseconds(10));
  }
}

void Engine::applyLocalSettings()
{
  const Settings s = getSettings();
  m_inputGain = s.inputVolume;
  // the plug-in plays its own metronome (click.h)
  m_client->config_metronome = 0.0f;
  m_client->config_metronome_mute = true;
  m_clickVolume = s.metronomeMute ? 0.0f : s.metronomeVolume;
  // the engine monitors the input itself (process()), so it goes on while the host is stopped
  m_client->SetLocalChannelMonitoring(0, true, 1.0f, true, 0.0f, true, true, false, false);
  m_monitor = s.monitor;
  m_transmit = s.transmit;
  applyLiveBuffer();

  applySendKbps();
  applyBroadcast();
}

void Engine::applyBroadcast()
{
  const bool want = getSettings().transmit && !m_hostStopped.load(std::memory_order_relaxed);
  bool bcast = false;
  m_client->GetLocalChannelInfo(0, nullptr, nullptr, &bcast);
  // from the next interval (the channel stays, so the others keep their settings for it)
  if (bcast != want) m_client->SetLocalChannelInfo(0, nullptr, false, 0, false, 0, true, want);
}

void Engine::doConnect(const std::string &serverIn, const std::string &userIn)
{
  std::string server = trim(serverIn);
  if (server.empty())
  {
    addChat(ChatLine::Error, "", "Type a server as host:port, or host one with \"Host a server\".");
    return;
  }
  if (server.find(':') == std::string::npos) server += ":2049";

  std::string user = trim(userIn);
  for (auto &c : user) if (c == '@' || c == ':' || (unsigned char)c < ' ') c = '_';
  if (user.empty()) user = "guest";
  if (user.size() > 32) user.resize(32);

  if (m_state == ConnState::Connecting || m_state == ConnState::Connected) clientDisconnect();

  m_server = server;
  m_appliedRemote.clear();
  m_warnedCantSend = false;
  m_state = ConnState::Connecting;
  m_connectTime = now_seconds();
  m_statusText = "Connecting to " + server + "...";
  addChat(ChatLine::System, "", m_statusText);

  // anonymous login, which most servers allow
  const std::string login = "anonymous:" + user;
  clientConnect(server, login);
}

void Engine::clientConnect(const std::string &server, const std::string &login)
{
  std::lock_guard<std::mutex> audio(m_audioMutex);
  m_client->Connect(server.c_str(), login.c_str(), "");
}

void Engine::clientDisconnect()
{
  stopLive(); // (it doesn't need the audio thread to wait)
  std::lock_guard<std::mutex> audio(m_audioMutex);
  m_client->Disconnect();
}

void Engine::doDisconnect(const char *why)
{
  if (m_state == ConnState::Connecting || m_state == ConnState::Connected)
  {
    clientDisconnect();
    addChat(ChatLine::System, "", why ? why : "Disconnected.");
  }
  m_state = ConnState::Disconnected;
  m_statusText = "Not connected";
}

void Engine::updateConnection()
{
  if (m_state != ConnState::Connecting && m_state != ConnState::Connected) return;
  const int st = m_client->GetStatus();
  if (st == NJClient::NJC_STATUS_OK)
  {
    if (m_state == ConnState::Connecting && m_client->GetBPI() > 0)
    {
      m_state = ConnState::Connected;
      m_connectedTime = now_seconds();
      m_statusText = "Connected to " + m_server + " as " + displayName(m_client->GetUser());
      addChat(ChatLine::System, "", m_statusText);
      // the room gets the host's tempo, if they differ, unless Live follows the room through Link
      m_armPending = true;
    }
    return;
  }
  if (st == NJClient::NJC_STATUS_PRECONNECT) return;

  const std::string err = m_client->GetErrorStr() ? m_client->GetErrorStr() : "";
  std::string text;
  if (st == NJClient::NJC_STATUS_CANTCONNECT) text = "Could not connect to " + m_server;
  else if (st == NJClient::NJC_STATUS_INVALIDAUTH) text = m_server + " refused the login";
  else text = (m_state == ConnState::Connected ? "Lost the connection to " : "Could not connect to ") + m_server;
  if (!err.empty()) text += ": " + err;
  clientDisconnect();
  m_state = ConnState::Failed;
  m_statusText = text;
  addChat(ChatLine::Error, "", text);
}

// The plug-in sends Opus or FLAC only. A server too old to know them (it sends no codec
// mask), or a room switched to Vorbis, can't hear it, and its players sound silent here.
void Engine::checkCanSend(bool canSend)
{
  // the codec mask comes just after the login; give it a moment
  if (now_seconds() - m_connectedTime < 1.5) return;
  if (!canSend && !m_warnedCantSend)
  {
    m_warnedCantSend = true;
    addChat(ChatLine::Error, "", "This server doesn't support Opus (its room plays Vorbis), so you can't send here, and the other "
                                 "players sound silent. If the server knows Opus, type !codec opus to switch the room.");
  }
  else if (canSend && m_warnedCantSend)
  {
    m_warnedCantSend = false;
    addChat(ChatLine::System, "", "The room plays " + codec_name(m_client->GetCodecMask()) + " now, so you're sending again.");
  }
}

// votes the room to the host's tempo when it changes (see TempoFollower)
void Engine::followHostTempo()
{
  if (!getSettings().followHostTempo) return;
  const double t = now_seconds();
  // the host's tempo, while it runs the audio
  const double tempo = t - m_lastAudio < 1.0 ? m_hostTempo.load(std::memory_order_relaxed) : 0.0;
  // no vote while it waits to see if Live follows the room through Link (updateLink)
  const int room = m_state == ConnState::Connected && !m_armPending ? m_client->GetServerBPM() : 0;
  const int bpm = m_tempoFollower.update(t, tempo, room);
  if (bpm <= 0) return;
  m_linkKeeper.voted(t, bpm);
  char buf[256];
  snprintf(buf, sizeof(buf), "!vote bpm %d", bpm);
  m_client->ChatMessage_Send("MSG", buf);
  std::string host;
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    host = shortHostName(m_hostName);
  }
  snprintf(buf, sizeof(buf), "%s is at %s BPM, so you vote the room to %d (!vote bpm %d).", host.c_str(), bpmText(tempo).c_str(), bpm, bpm);
  addChat(ChatLine::System, "", buf);
}

void Engine::releaseLink()
{
  if (!m_link->driving()) return;
  {
    std::lock_guard<std::mutex> audio(m_audioMutex); // not during process()
    m_link->release();
  }
  m_linkLeads = false;
}

// Ableton Link: in it while Sync is on in a room, and leading it unless another instance in this
// program does. Not in Link out of a room: Live, turning its Link on, would take Link's tempo then.
// process() does the tempo and the beat; this decides whether joining a room votes for the host's
// tempo, and puts the room's tempo back on Link when it should (LinkTempoKeeper).
void Engine::updateLink()
{
  const double t = now_seconds();
  if (t - m_lastLinkPoll < LINK_POLL) return;
  m_lastLinkPoll = t;

  const bool sync = getSettings().followHostTempo;
  const bool inRoom = m_state == ConnState::Connected;
  if (!sync || !inRoom) releaseLink();
  m_link->use(sync && inRoom);
  if (!sync || !inRoom)
  {
    m_linkState = sync ? LinkState::Waiting : LinkState::Off;
    m_linkPeers = 0;
    m_linkTempo = 0;
    return;
  }
  if (!m_link->driving() && m_link->drive())
  {
    m_linkKeeper.reset();
    m_toldContested = m_toldLinkLeads = false;
  }
  const bool driving = m_link->driving();
  m_linkPeers = m_link->numPeers();
  m_linkTempo = m_link->sessionTempo();
  if (!LinkSync::enabledInProgram()) m_linkState = LinkSync::unavailable() ? LinkState::Unavailable : LinkState::Off;
  else if (driving) m_linkState = m_link->contested() ? LinkState::Contested : LinkState::Leading;
  else m_linkState = LinkState::OtherInstance;
  const bool leads = driving && m_linkPeers > 0 && !m_link->contested();

  std::string host;
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    host = m_hostName;
  }
  const std::string live = shortHostName(host);

  // Joined a room, or Sync went on: today's vote for the host's tempo, unless Live takes the room's
  // through Link. Link finds the other apps soon after it goes on, so it waits for that.
  if (m_armPending && (!driving || m_linkPeers > 0 || LinkSync::enabledFor() >= LINK_DISCOVERY))
  {
    m_armPending = false;
    if (leads) m_tempoFollower.disarm();
    else m_tempoFollower.arm();
  }

  // the room's tempo back on Link, when the room didn't take Live's or another app set its own
  if (leads && t - m_lastAudio < 1.0 && m_client->IsIntervalRunning() && !m_link->restorePending())
  {
    const double hostTempo = m_hostTempo.load(std::memory_order_relaxed);
    const int room = m_client->GetServerBPM();
    const LinkTempoKeeper::Action a = m_linkKeeper.update(t, m_linkTempo, m_client->GetActualBPM(), room, hostTempo,
                                                          m_tempoFollower.changedAt(), m_linkPeers);
    if (a.kind == LinkTempoKeeper::Action::Restore)
    {
      m_link->restoreTempo();
      char buf[512];
      buf[0] = 0;
      if (a.why == LinkTempoKeeper::Action::NotTaken)
        snprintf(buf, sizeof(buf), "The room stays at %d BPM, so %s goes back to it.", room, live.c_str());
      else if (a.why == LinkTempoKeeper::Action::OutOfRange)
        snprintf(buf, sizeof(buf), "A NINJAM room can't have %s BPM (only %d to %d), so %s goes back to the room's %d.",
                 bpmText(m_linkTempo).c_str(), TempoFollower::MIN_BPM, TempoFollower::MAX_BPM, live.c_str(), room);
      if (buf[0]) addChat(ChatLine::System, "", buf);
    }
    if (m_linkKeeper.contested()) m_link->setContested();
  }
  if (leads && !m_toldLinkLeads && m_client->IsIntervalRunning())
  {
    m_toldLinkLeads = true;
    char buf[256];
    snprintf(buf, sizeof(buf), "Link: %s takes the room's tempo (%d BPM), and its bars start with the intervals.", live.c_str(),
             m_client->GetServerBPM());
    addChat(ChatLine::System, "", buf);
  }
  if (driving && m_link->contested() && !m_toldContested)
  {
    m_toldContested = true;
    addChat(ChatLine::Error, "", "Another app on the network keeps changing Link's tempo or beat (another Jamsa?), so this one "
                                 "stopped leading Link. Turn Sync off and on to lead it again.");
  }

  // no other Link app: Live's Link is off
  if (driving && m_linkPeers == 0 && !m_toldLinkHint && LinkSync::enabledFor() >= LINK_HINT_DELAY &&
      t - m_connectedTime >= LINK_HINT_DELAY)
  {
    m_toldLinkHint = true;
    addChat(ChatLine::System, "", linkHint(host));
  }
}

// ---------------------------------------------------------------------------
// Live mode (network thread)
// ---------------------------------------------------------------------------

// Stops the Live client and forgets what it knew about this connection's room. NJClient's
// players go back to their intervals with their slots (NJClient forgets them on Disconnect()).
void Engine::stopLive()
{
  m_liveOn = false; // the audio thread stops feeding it, and plays intervals
  if (m_live->running()) m_live->stop();
  m_liveGen = 0;
  m_liveUsers.clear();
  m_liveDropoutsBase.clear();
  m_toldLiveOffered = m_toldLiveNoAnswer = m_toldLiveError = false;
  m_liveRetryAt = 0;
}

// how late we hear each player, in whole ms
static std::string ms_text(double ms)
{
  char buf[32];
  snprintf(buf, sizeof(buf), "%.0f ms", ms);
  return buf;
}

int Engine::liveBufferFor(int askedMs, int hostBlock, int sampleRate)
{
  int ms = std::max(askedMs, Settings::MIN_LIVE_BUFFER_MS);
  if (hostBlock > 0 && sampleRate > 0) ms = std::max(ms, (int)ceil(2000.0 * hostBlock / sampleRate + 5.0));
  return std::min(ms, Settings::MAX_LIVE_BUFFER_MS);
}

void Engine::applyLiveBuffer()
{
  // the host's longest block in the last half minute (a host may change its block size)
  const double t = now_seconds();
  const int frames = m_hostFrames.load(std::memory_order_relaxed);
  if (frames >= m_liveHostBlock) m_liveHostBlockAt = t;
  if (frames > m_liveHostBlock || (frames > 0 && t - m_liveHostBlockAt > 30.0))
  {
    m_liveHostBlock = frames;
    m_liveHostBlockAt = t;
  }
  const int ms = liveBufferFor(getSettings().liveBufferMs, m_liveHostBlock, m_sampleRate.load(std::memory_order_relaxed));
  if (ms == m_liveBufferApplied) return;
  // a new jitter buffer: each stream starts again (a short gap), and the dropouts count again
  m_live->setJitterMs(ms);
  m_liveBufferApplied = ms;
  m_liveBufferChangedAt = t;
  m_liveCountSince = t;
  m_liveDropoutsBase.clear();
  if (m_live->running())
    for (auto &ps : m_live->stats()) m_liveDropoutsBase[ps.name] = ps.underruns + ps.overruns + ps.dropped;
}

// From what the server says (MESSAGE_SERVER_LIVE, through NJClient): starts the Live client,
// keeps it up to date (the room's Live on or off, its bitrate and frame, whether we may send, the
// other players), and decides which players NJClient plays live from the next interval on: those
// whose slot plays well, while the room's Live is on.
void Engine::updateLive()
{
  applyLiveBuffer();
  const int gen = m_state == ConnState::Connected ? m_client->GetLiveGeneration() : 0;
  if (!gen)
  {
    // not connected, or a server without Live (or it hasn't said yet)
    if (m_live->running() || m_liveOn) stopLive();
    return;
  }
  const double t = now_seconds();
  const int flags = m_client->GetLiveFlags();
  jamsa_live::Config c;
  c.myId = m_client->GetLivePeerId();
  c.key = m_client->GetLiveKey();
  c.serverIp = m_client->GetServerAddress();
  c.udpPort = (uint16_t)(m_client->GetLiveUdpPort() > 0 ? m_client->GetLiveUdpPort() : m_client->GetServerPort());
  if (m_client->GetLiveKbps() > 0) c.kbps = m_client->GetLiveKbps();
  if (m_client->GetLiveFrameSamples() > 0) c.frameSamples = m_client->GetLiveFrameSamples();
  c.udpOpen = (flags & mpb_server_live::LIVE_FLAG_UDP_OPEN) != 0;
  c.roomOn = (flags & mpb_server_live::LIVE_FLAG_ROOM_ON) != 0;
  // we send while the server takes it, transmit is on and the host plays, as with intervals (but
  // nothing at all while not: no stream of silence)
  c.maySend = (flags & mpb_server_live::LIVE_FLAG_MAY_SEND) && getSettings().transmit &&
              !m_hostStopped.load(std::memory_order_relaxed) && !m_offline.load(std::memory_order_relaxed);

  if (!c.udpOpen)
  {
    // the server's Live is off (its UDP port closed, or a new config turned it off)
    if (m_live->running())
    {
      m_liveOn = false;
      m_live->stop();
      m_liveGen = 0;
    }
  }
  else if (!m_live->running())
  {
    if (t < m_liveRetryAt) return;
    std::string err;
    if (!m_live->start(c, &err))
    {
      m_liveRetryAt = t + LIVE_RETRY;
      if (!m_toldLiveError)
      {
        m_toldLiveError = true;
        addChat(ChatLine::Error, "", "Live mode can't start here (" + err + "), so you hear everyone through intervals.");
      }
      return;
    }
    m_liveStartedAt = t;
    m_liveGen = 0; // the players again
  }
  else m_live->update(c); // (a new id or key from the server restarts it)

  const bool running = m_live->running();
  if (running && m_liveGen != gen)
  {
    m_liveGen = gen;
    std::vector<jamsa_live::Peer> peers;
    for (int i = 0; ; i ++)
    {
      unsigned int id = 0;
      int pf = 0;
      const char *name = m_client->GetLivePeer(i, &id, &pf);
      if (!name) break;
      jamsa_live::Peer p;
      p.id = id;
      p.name = name;
      p.bound = (pf & mpb_server_live::LIVE_PEER_BOUND) != 0;
      peers.push_back(p);
    }
    m_live->setPeers(peers);
  }
  if (running && t - m_lastLivePoll >= LIVE_POLL)
  {
    m_lastLivePoll = t;
    m_live->poll();
  }
  const bool roomOn = running && c.roomOn;
  m_liveOn = roomOn;

  if (running && !c.roomOn && !m_toldLiveOffered)
  {
    m_toldLiveOffered = true;
    addChat(ChatLine::System, "", "This server has Live mode: type !live on, and the Jamsa players in the room hear each other "
                                  "in real time (it needs UDP to the server's port " + std::to_string(c.udpPort) + ").");
  }
  if (roomOn && m_live->serverRttMs() <= 0 && t - m_liveStartedAt >= LIVE_NO_ANSWER && !m_toldLiveNoAnswer)
  {
    m_toldLiveNoAnswer = true;
    addChat(ChatLine::Error, "", "Live: the server doesn't answer on UDP port " + std::to_string(c.udpPort) +
                                 ", so you hear everyone through intervals. A firewall or router may block UDP; whoever "
                                 "hosts the server needs UDP port " + std::to_string(c.udpPort) + " forwarded too.");
  }

  // Each player: live from the next interval while their slot plays well (after it played badly,
  // once it has played well for a few seconds); back to intervals from the next one when it
  // doesn't (NJClient plays the intervals at once while the room's Live is off: liveAudio()).
  std::vector<jamsa_live::PeerStats> stats;
  bool haveStats = false;
  std::set<std::string> present;
  const int n = m_client->GetNumUsers();
  for (int u = 0; u < n; u ++)
  {
    const char *un = m_client->GetUserState(u);
    if (!un) continue;
    const std::string name = un;
    present.insert(name);
    LiveUser &lu = m_liveUsers[name];
    int slot = -1;
    if (roomOn)
      for (int i = 0; ; i ++)
      {
        unsigned int id = 0;
        const char *pn = m_client->GetLivePeer(i, &id, nullptr);
        if (!pn) break;
        if (name == pn)
        {
          slot = m_live->slotOf(id);
          break;
        }
      }
    const bool healthy = slot >= 0 && m_live->slotHealthy(slot);
    if (!healthy) lu.healthySince = -1;
    else if (lu.healthySince < 0) lu.healthySince = t;
    const double settle = lu.fellBackAt >= 0 && t - lu.fellBackAt < 60.0 ? LIVE_RECOVER : 0.0;
    bool want = healthy && t - lu.healthySince >= settle;
    // the stream starting again with a new jitter buffer (/live buffer N): a gap, not dropouts
    if (!want && lu.want && slot >= 0 && slot == lu.slot && t - m_liveBufferChangedAt < LIVE_REBUFFER) want = true;
    if (lu.want && !want && roomOn) lu.fellBackAt = t; // it played badly
    if (want != lu.want || (want && slot != lu.slot))
    {
      m_client->SetUserLive(u, want ? slot : -1);
      lu.want = want;
      lu.slot = want ? slot : -1;
    }

    // what NJClient plays now (it changes at an interval's start)
    const int on = m_client->GetUserLive(u);
    if ((on >= 0) != (lu.on >= 0))
    {
      if (on >= 0)
      {
        if (!haveStats)
        {
          stats = m_live->stats();
          haveStats = true;
        }
        std::string late;
        for (auto &ps : stats)
          if (ps.name == name && ps.oneWayMs > 0) late = ", about " + ms_text(ps.oneWayMs) + " late";
        addChat(ChatLine::System, "", "You hear " + displayName(name) + " live now" + late + ".");
      }
      else if (roomOn)
        addChat(ChatLine::System, "", displayName(name) + " is back on intervals: their real-time audio stopped or had dropouts.");
    }
    lu.on = on;
  }
  for (auto it = m_liveUsers.begin(); it != m_liveUsers.end(); )
    if (!present.count(it->first)) it = m_liveUsers.erase(it);
    else ++it;
}

void Engine::updateLiveView(Snapshot &snap)
{
  const Settings s = getSettings();
  snap.liveBufferMs = s.liveBufferMs;
  snap.liveBufferInUseMs = m_liveBufferApplied;
  snap.liveHostBlock = m_liveHostBlock;
  if (m_state != ConnState::Connected || !m_client->GetLiveGeneration()) return;
  const int flags = m_client->GetLiveFlags();
  snap.liveOffered = (flags & mpb_server_live::LIVE_FLAG_UDP_OPEN) != 0;
  snap.liveRoomOn = snap.liveOffered && (flags & mpb_server_live::LIVE_FLAG_ROOM_ON);
  snap.liveMaySend = snap.liveRoomOn && (flags & mpb_server_live::LIVE_FLAG_MAY_SEND);
  snap.liveRunning = m_live->running();
  snap.liveSending = snap.liveRunning && snap.liveMaySend && s.transmit && !m_hostStopped.load(std::memory_order_relaxed) &&
                     !m_offline.load(std::memory_order_relaxed);
  snap.liveKbps = m_client->GetLiveKbps();
  snap.liveFrame = m_client->GetLiveFrameSamples();
  if (!snap.liveRunning) return;
  snap.liveServerRttMs = m_live->serverRttMs();
  const jamsa_live::Counters k = m_live->counters();
  snap.liveSent = k.sent;
  snap.liveReceived = k.received;
  for (const jamsa_live::PeerStats &ps : m_live->stats())
  {
    LivePeerView v;
    v.name = ps.name;
    v.displayName = displayName(ps.name);
    v.slot = ps.slot;
    for (int i = 0; ; i ++)
    {
      unsigned int id = 0;
      int pf = 0;
      if (!m_client->GetLivePeer(i, &id, &pf)) break;
      if (id == ps.id) v.bound = (pf & mpb_server_live::LIVE_PEER_BOUND) != 0;
    }
    v.streaming = ps.streaming;
    v.healthy = ps.healthy;
    auto lu = m_liveUsers.find(ps.name);
    v.live = lu != m_liveUsers.end() && lu->second.on >= 0;
    v.delayMs = ps.oneWayMs;
    v.rttMs = ps.rttMs;
    v.bufferMs = ps.bufferMs;
    v.codecMs = ps.codecMs;
    v.lossPct = ps.lossPct;
    v.underruns = ps.underruns;
    v.overruns = ps.overruns;
    v.dropped = ps.dropped;
    v.resent = ps.resent;
    snap.livePeers.push_back(v);
  }
}

// "/live": how each Live player sounds; "/live buffer N": the jitter buffer (ms)
void Engine::liveChatCommand(const std::string &argIn)
{
  std::string arg = argIn;
  for (auto &ch : arg) ch = (char)tolower((unsigned char)ch);
  const double t = now_seconds();
  const int buffer = getSettings().liveBufferMs;
  if (arg == "on" || arg == "off")
  {
    if (m_state == ConnState::Connected) m_client->ChatMessage_Send("MSG", arg == "on" ? "!live on" : "!live off");
    else addChat(ChatLine::Error, "", "Not connected: Live mode is the room's, so join a room first.");
    return;
  }
  if (arg.compare(0, 6, "buffer") == 0)
  {
    const std::string v = trim(arg.substr(6));
    if (v.empty())
    {
      addChat(ChatLine::System, "", "Live's jitter buffer is " + std::to_string(buffer) + " ms. Type /live buffer 30 to change it (" +
                                    std::to_string(Settings::MIN_LIVE_BUFFER_MS) + " to " + std::to_string(Settings::MAX_LIVE_BUFFER_MS) + ").");
      return;
    }
    char *end = nullptr;
    const long ms = strtol(v.c_str(), &end, 10);
    if (!end || *end || ms < Settings::MIN_LIVE_BUFFER_MS || ms > Settings::MAX_LIVE_BUFFER_MS)
    {
      addChat(ChatLine::Error, "", "/live buffer takes a number of ms from " + std::to_string(Settings::MIN_LIVE_BUFFER_MS) + " to " +
                                   std::to_string(Settings::MAX_LIVE_BUFFER_MS) + ", such as /live buffer 30.");
      return;
    }
    setLiveBufferMs((int)ms); // the dropouts count again from there (applyLocalSettings())
    std::string text = "Live's jitter buffer is " + std::to_string(ms) + " ms now (it was " + std::to_string(buffer) +
                       "). Less is less late, more has fewer dropouts: type /live to see them.";
    const int use = liveBufferFor((int)ms, m_liveHostBlock, m_sampleRate.load(std::memory_order_relaxed));
    if (use > ms)
      text += " Your DAW's audio buffer (" + std::to_string(m_liveHostBlock) + " samples) needs at least " + std::to_string(use) +
              " ms, so Live uses " + std::to_string(use) + " ms.";
    addChat(ChatLine::System, "", text);
    return;
  }
  if (!arg.empty())
  {
    addChat(ChatLine::Error, "", "Type /live to see how the Live players sound, or /live buffer 30 to set Live's jitter buffer (ms).");
    return;
  }

  if (m_state != ConnState::Connected)
  {
    addChat(ChatLine::System, "", "Not connected. In a room with Live mode, /live shows how late you hear each Jamsa player. Live's "
                                  "jitter buffer is " + std::to_string(buffer) + " ms (/live buffer N to change it).");
    return;
  }
  const int flags = m_client->GetLiveGeneration() ? m_client->GetLiveFlags() : 0;
  if (!(flags & mpb_server_live::LIVE_FLAG_UDP_OPEN))
  {
    addChat(ChatLine::System, "", m_client->GetLiveGeneration() ? "This server's Live mode is off, so everyone plays through intervals."
                                                                 : "This server doesn't have Live mode, so everyone plays through intervals.");
    return;
  }
  if (!(flags & mpb_server_live::LIVE_FLAG_ROOM_ON))
  {
    addChat(ChatLine::System, "", "Live is off in this room. Type !live on, and the Jamsa players here hear each other in real time.");
    return;
  }
  if (!m_live->running())
  {
    addChat(ChatLine::System, "", "Live is on in this room, but yours couldn't start, so you hear everyone through intervals.");
    return;
  }
  const Settings s = getSettings();
  std::string head = "Live is on. Jitter buffer " + std::to_string(m_liveBufferApplied) + " ms";
  if (m_liveBufferApplied > s.liveBufferMs)
    head += ": your DAW's audio buffer (" + std::to_string(m_liveHostBlock) + " samples) needs that much, more than the " +
            std::to_string(s.liveBufferMs) + " ms you set. ";
  else head += " (/live buffer N to change it). ";
  const double rtt = m_live->serverRttMs();
  if (rtt > 0)
  {
    char buf[96];
    snprintf(buf, sizeof(buf), "Round trip to the server %.1f ms.", rtt);
    head += buf;
  }
  else head += "The server doesn't answer over UDP yet.";
  if (!(flags & mpb_server_live::LIVE_FLAG_MAY_SEND))
    head += " The room has no room for more Live players, so the others hear you through intervals.";
  else if (!s.transmit) head += " Transmit is off, so you send nothing.";
  else if (m_hostStopped.load(std::memory_order_relaxed)) head += " The host is stopped, so you send nothing until it plays.";
  addChat(ChatLine::System, "", head);

  const double since = m_liveCountSince > 0 ? t - m_liveCountSince : -1;
  std::set<std::string> peers;
  for (const jamsa_live::PeerStats &ps : m_live->stats())
  {
    peers.insert(ps.name);
    auto lu = m_liveUsers.find(ps.name);
    const bool live = lu != m_liveUsers.end() && lu->second.on >= 0;
    std::string line = displayName(ps.name) + ": ";
    if (ps.slot < 0) line += "intervals (Jamsa hears " + std::to_string(jamsa_live::Client::MAX_PEERS) + " Live players at most)";
    else if (!ps.streaming) line += "intervals (no real-time audio from them)";
    else
    {
      line += live ? "live" : ps.healthy ? "live from the next interval" : "intervals (their real-time audio has dropouts)";
      char buf[256];
      snprintf(buf, sizeof(buf), ", about %.0f ms late (buffer %.0f ms, network %.0f ms, codec %.0f ms), %.1f%% lost", ps.oneWayMs,
               ps.bufferMs, ps.rttMs / 2, ps.codecMs, ps.lossPct);
      line += buf;
    }
    if (ps.slot >= 0)
    {
      const int all = ps.underruns + ps.overruns + ps.dropped;
      auto base = m_liveDropoutsBase.find(ps.name);
      const int from = base != m_liveDropoutsBase.end() && base->second <= all ? base->second : 0;
      char buf[256];
      if (since >= 0)
        snprintf(buf, sizeof(buf), ". Dropouts: %d in the %.0f s since the buffer changed, %d in all (%d ran dry, %d overflowed, %d lost)",
                 all - from, since, all, ps.underruns, ps.overruns, ps.dropped);
      else
        snprintf(buf, sizeof(buf), ". Dropouts: %d (%d ran dry, %d overflowed, %d lost)", all, ps.underruns, ps.overruns, ps.dropped);
      line += buf;
    }
    addChat(ChatLine::System, "", line + ".");
  }
  // the others in the room play through intervals
  const int n = m_client->GetNumUsers();
  for (int u = 0; u < n; u ++)
  {
    const char *un = m_client->GetUserState(u);
    if (un && !peers.count(un)) addChat(ChatLine::System, "", displayName(un) + ": intervals (not a Live player: an older client, or the room is full)");
  }
}

void Engine::doSetHosting(bool on)
{
  if (on == m_hosting) return;
  m_hostBusy = true;
  publish();

  if (on)
  {
    std::string err;
    if (HostServer::instance().start(this, getSettings().opusKbps, &err))
    {
      m_hosting = true;
      m_hostLogSerial = 0;
      m_hostPort = HostServer::instance().state().port;
      addChat(ChatLine::System, "", "Hosting a server on port " + std::to_string(m_hostPort) + ".");
      doConnect("localhost:" + std::to_string(m_hostPort), getSettings().userName);
    }
    else addChat(ChatLine::Error, "", err);
  }
  else
  {
    if (m_server == "localhost:" + std::to_string(m_hostPort)) doDisconnect("Disconnected from your server.");
    addChat(ChatLine::System, "", "Stopping the server...");
    publish();
    HostServer::instance().stop(this);
    m_hosting = false;
    m_hostPort = 0;
    addChat(ChatLine::System, "", "The server is stopped.");
  }
  m_hostBusy = false;
}

int Engine::findUser(const std::string &name) const
{
  const int n = m_client->GetNumUsers();
  for (int u = 0; u < n; u ++)
  {
    const char *un = m_client->GetUserState(u);
    if (un && name == un) return u;
  }
  return -1;
}

void Engine::updateUsers(Snapshot &snap)
{
  m_client->HasUserInfoChanged();
  Settings settings;
  bool haveSettings = false;

  std::string signature;
  bool anySolo = false;
  const int n = m_client->GetNumUsers();
  for (int u = 0; u < n; u ++)
  {
    const char *un = m_client->GetUserState(u);
    RemoteUserView uv;
    uv.name = un ? un : "";
    uv.displayName = displayName(uv.name);
    signature += uv.name + "\n";
    for (int i = 0; ; i ++)
    {
      const int ch = m_client->EnumUserChannels(u, i);
      if (ch < 0) break;
      RemoteChannelView cv;
      cv.index = ch;
      const std::string key = remoteKey(uv.name, ch);
      if (!m_appliedRemote.count(key))
      {
        // a channel we haven't seen this session gets what was saved for it
        m_appliedRemote.insert(key);
        if (!haveSettings)
        {
          settings = getSettings();
          haveSettings = true;
        }
        auto it = settings.remote.find(key);
        if (it != settings.remote.end())
          m_client->SetUserChannelState(u, ch, false, false, true, it->second.volume, true, it->second.pan, true, it->second.mute, false, false);
      }
      bool sub = false, mute = false, solo = false;
      int outch = 0;
      const char *cn = m_client->GetUserChannelState(u, ch, &sub, &cv.volume, &cv.pan, &mute, &solo, &outch);
      cv.name = cn ? cn : "";
      cv.mute = mute;
      cv.solo = solo;
      if (solo) anySolo = true;
      cv.playerOut = outch >= 2 && !(outch & 1024) ? outch / 2 : 0;
      if (cv.playerOut > 0)
      {
        int &said = m_playerOutSaid[uv.name];
        if (said != cv.playerOut)
        {
          said = cv.playerOut;
          addChat(ChatLine::System, "", uv.displayName + " plays alone on the output Player " + std::to_string(cv.playerOut));
        }
      }
      cv.peak[0] = m_client->GetUserChannelPeak(u, ch, 0);
      cv.peak[1] = m_client->GetUserChannelPeak(u, ch, 1);
      // Live mode: their real-time audio plays this channel
      if (ch == 0 && m_client->GetUserLive(u) >= 0)
      {
        cv.live = true;
        cv.name += " \xc2\xb7 live";
      }
      signature += std::to_string(ch) + ":" + cv.name + "\n";
      uv.channels.push_back(cv);
    }
    snap.users.push_back(uv);
  }
  m_anySolo = anySolo;

  {
    std::lock_guard<std::mutex> lock(m_snapMutex);
    // compare with what was published, by structure
    std::string old;
    for (auto &u : m_snap.users)
    {
      old += u.name + "\n";
      for (auto &c : u.channels) old += std::to_string(c.index) + ":" + c.name + "\n";
    }
    if (old != signature) m_usersRevision ++;
  }
  snap.usersRevision = m_usersRevision;
}

void Engine::pollHosting(Snapshot &snap)
{
  HostServer::State st = HostServer::instance().state();
  if (m_hosting && st.owner == this)
  {
    snap.hosting = true;
    snap.hostPort = st.port;
    snap.hostPublicAddress = st.publicAddress;
    snap.hostLocalAddress = st.localAddress;
    snap.hostNote = st.portmapNote;
    snap.hostPortmapPending = st.portmapPending;
    snap.hostLookedUpAddress = st.lookedUpAddress;
    snap.hostLookupPending = st.lookupPending;
    snap.hostJoinedFromInternet = st.joinedFromInternet;
    snap.hostUsers = st.users;
    for (const std::string &m : HostServer::instance().messagesSince(&m_hostLogSerial))
      addChat(ChatLine::Server, "", m);
  }
  else if (st.owner && st.owner != this) snap.hostedElsewherePort = st.port;
  snap.hostBusy = m_hostBusy;
}

void Engine::publish()
{
  Snapshot snap;
  snap.state = m_state;
  snap.status = m_statusText.empty() ? "Not connected" : m_statusText;
  snap.server = m_server;
  snap.opusKbps = getSettings().opusKbps;
  snap.sendKbps = sendKbps();
  if (m_state == ConnState::Connected || m_state == ConnState::Connecting)
  {
    snap.userName = displayName(m_client->GetUser() ? m_client->GetUser() : "");
    // what the server set: GetBPI() changes only at the next interval, and only while the host runs the audio
    snap.bpm = (float)m_client->GetServerBPM();
    snap.bpi = m_client->GetServerBPI();
    m_client->GetPosition(&snap.intervalPos, &snap.intervalLength);
    snap.intervalBeats = m_client->GetBPI();
    if (m_state == ConnState::Connected)
    {
      snap.roomCodec = codec_name(m_client->GetCodecMask());
      snap.roomOpusKbps = m_client->GetRoomOpusKbps();
      const unsigned int upload = m_client->GetUploadCodec();
      if (upload) snap.uploadCodec = fourcc_text(upload);
      snap.canSend = upload != 0;
      checkCanSend(upload != 0);
    }
    updateUsers(snap);
  }
  else
  {
    m_anySolo = false;
    m_playerOutSaid.clear();
    std::lock_guard<std::mutex> lock(m_snapMutex);
    if (!m_snap.users.empty()) m_usersRevision ++;
    snap.usersRevision = m_usersRevision;
  }
  const unsigned int calls = m_processCalls.load(std::memory_order_relaxed);
  const double t = now_seconds();
  if (calls != m_seenProcessCalls)
  {
    m_seenProcessCalls = calls;
    m_lastAudio = t;
  }
  snap.audioRunning = t - m_lastAudio < 1.0;
  snap.localPeak[0] = m_client->GetLocalChannelPeak(0, 0);
  snap.localPeak[1] = m_client->GetLocalChannelPeak(0, 1);
  snap.sendPaused = snap.audioRunning && m_hostStopped.load(std::memory_order_relaxed) && getSettings().transmit;
  updateLiveView(snap);

  // following the host, and its tempo against the room's
  std::string hostName;
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    snap.followTempo = m_settings.followHostTempo;
    hostName = m_hostName;
  }
  snap.hostLabel = shortHostName(hostName);
  snap.hostChip = hostChipName(hostName);
  {
    const int ts = m_timeSig.load(std::memory_order_relaxed);
    snap.timeSigNum = ts / 256;
    snap.timeSigDen = ts % 256;
  }
  snap.hostTempo = snap.audioRunning ? m_hostTempo.load(std::memory_order_relaxed) : 0.0;
  snap.barState = snap.followTempo && m_state == ConnState::Connected ? (BarSync::State)m_barState.load(std::memory_order_relaxed) : BarSync::Off;
  // where the next interval starts in the host's bars, while they line up
  snap.nextIntervalPpq = snap.barState == BarSync::Aligned || snap.barState == BarSync::Lining
                         ? m_nextIntervalPpq.load(std::memory_order_relaxed) : -1;
  snap.barErrorMs = m_barErrorMs.load(std::memory_order_relaxed);
  snap.barFixes = m_barFixes.load(std::memory_order_relaxed);
  snap.tempoVotes = m_tempoFollower.votes();

  // Link
  snap.linkState = m_linkState;
  snap.linkPeers = (int)m_linkPeers;
  snap.linkTempo = m_linkTempo;
  const bool leading = m_linkState == LinkState::Leading;
  snap.linkLeads = leading && m_linkPeers > 0;
  snap.linkAligned = leading && m_link->aligned();
  snap.linkErrorMs = leading ? m_link->lastErrorMs() : 0.0;
  snap.linkLatencyMs = m_link->latencyMs();
  snap.linkJumps = m_link->jumps();
  snap.linkSteps = m_link->steps();
  snap.linkRestores = m_linkKeeper.restores();

  const int roomBpm = m_state == ConnState::Connected ? m_client->GetServerBPM() : 0;
  // not while the room's tempo changes to the host's (at the next interval), nor while Link's
  // tempo isn't the room's yet (the room votes on it, or Link gets the room's back)
  const bool linkSettles = snap.linkLeads && fabs(m_linkTempo - m_client->GetActualBPM()) > LinkTempoKeeper::SAME;
  const bool differs = snap.hostTempo > 0 && roomBpm > 0 && fabs(snap.hostTempo - roomBpm) > BarSync::SAME_TEMPO &&
                       fabs(snap.hostTempo - m_client->GetActualBPM()) > BarSync::SAME_TEMPO && !linkSettles;
  if (!differs) m_noteSince = -1;
  else if (m_noteSince < 0) m_noteSince = t;
  // where Link would lead the host, the note says to turn it on there
  const bool suggestLink = m_linkState == LinkState::Leading;
  if (differs && t - m_noteSince >= SYNC_NOTE_DELAY)
    snap.syncNote = hostTempoNote(hostName, snap.hostTempo, roomBpm, snap.followTempo, suggestLink);

  pollHosting(snap);

  {
    std::lock_guard<std::mutex> lock(m_chatMutex);
    snap.chatRevision = m_chatRevision;
  }
  {
    std::lock_guard<std::mutex> lock(m_settingsMutex);
    snap.settingsRevision = m_settingsRevision;
  }

  std::lock_guard<std::mutex> lock(m_snapMutex);
  snap.licensePending = m_snap.licensePending;
  snap.licenseText = m_snap.licenseText;
  m_snap = std::move(snap);
}

int Engine::licenseCallback(void *user, const char *text)
{
  // called from NJClient::Run() on the network thread, which waits here for the UI
  Engine *_this = (Engine *)user;
  _this->m_licenseAnswer = 0;
  _this->m_licenseWaiting = true;
  {
    std::lock_guard<std::mutex> lock(_this->m_snapMutex);
    _this->m_snap.licensePending = true;
    _this->m_snap.licenseText = text ? text : "";
  }
  _this->addChat(ChatLine::System, "", "The server asks you to accept its license.");

  const double deadline = now_seconds() + LICENSE_WAIT_SECONDS;
  while (!_this->m_quit && _this->m_licenseAnswer == 0 && now_seconds() < deadline)
  {
    std::unique_lock<std::mutex> lock(_this->m_cmdMutex);
    _this->m_cmdCv.wait_for(lock, std::chrono::milliseconds(50));
  }
  const bool accepted = _this->m_licenseAnswer == 1;
  _this->m_licenseWaiting = false;
  {
    std::lock_guard<std::mutex> lock(_this->m_snapMutex);
    _this->m_snap.licensePending = false;
    _this->m_snap.licenseText.clear();
  }
  if (!accepted) _this->addChat(ChatLine::System, "", "You did not accept the license, so the server will not let you in.");
  return accepted ? 1 : 0;
}

void Engine::chatCallback(void *user, NJClient *, const char **parms, int nparms)
{
  Engine *_this = (Engine *)user;
  if (nparms < 1 || !parms[0]) return;
  const std::string cmd = parms[0];
  const std::string p1 = nparms > 1 && parms[1] ? parms[1] : "";
  const std::string p2 = nparms > 2 && parms[2] ? parms[2] : "";

  if (cmd == "MSG")
  {
    if (p1.empty()) _this->addChat(ChatLine::Server, "", p2);
    else if (p2.compare(0, 4, "/me ") == 0) _this->addChat(ChatLine::Action, displayName(p1), p2.substr(4));
    else _this->addChat(ChatLine::Message, displayName(p1), p2);
  }
  else if (cmd == "PRIVMSG")
  {
    _this->addChat(ChatLine::Private, displayName(p1), p2);
  }
  else if (cmd == "TOPIC")
  {
    if (p1.empty()) { if (!p2.empty()) _this->addChat(ChatLine::Topic, "", "Topic: " + p2); }
    else _this->addChat(ChatLine::Topic, "", displayName(p1) + " sets the topic to: " + p2);
  }
  else if (cmd == "JOIN" && !p1.empty())
  {
    _this->addChat(ChatLine::System, "", displayName(p1) + " joined");
  }
  else if (cmd == "PART" && !p1.empty())
  {
    _this->addChat(ChatLine::System, "", displayName(p1) + " left");
  }
}

// ---------------------------------------------------------------------------
// text commands
// ---------------------------------------------------------------------------

static const char *state_name(ConnState s)
{
  switch (s)
  {
    case ConnState::Connecting: return "connecting";
    case ConnState::Connected: return "connected";
    case ConnState::Failed: return "failed";
    default: return "disconnected";
  }
}

static const char *link_state_name(LinkState s)
{
  switch (s)
  {
    case LinkState::Waiting: return "waiting";
    case LinkState::Leading: return "leading";
    case LinkState::OtherInstance: return "other";
    case LinkState::Contested: return "contested";
    case LinkState::Unavailable: return "unavailable";
    default: return "off";
  }
}

static const char *bar_state_name(BarSync::State s)
{
  switch (s)
  {
    case BarSync::NoPosition: return "noposition";
    case BarSync::Stopped: return "stopped";
    case BarSync::OtherTempo: return "othertempo";
    case BarSync::Lining: return "lining";
    case BarSync::Aligned: return "aligned";
    default: return "off";
  }
}

std::string Engine::command(const std::string &lineIn)
{
  const std::string line = trim(lineIn);
  const size_t sp = line.find(' ');
  const std::string verb = line.substr(0, sp), arg = sp == std::string::npos ? "" : trim(line.substr(sp));

  if (verb == "connect")
  {
    // connect host:port [name]
    const size_t s2 = arg.find(' ');
    const std::string server = arg.substr(0, s2), name = s2 == std::string::npos ? getSettings().userName : trim(arg.substr(s2));
    connect(server, name);
    return "ok";
  }
  if (verb == "disconnect") { disconnect(); return "ok"; }
  if (verb == "host") { setHosting(arg == "on" || arg == "1"); return "ok"; }
  if (verb == "chat") { sendChat(arg); return "ok"; }
  if (verb == "license") { answerLicense(arg == "accept"); return "ok"; }
  if (verb == "transmit") { setTransmit(arg == "on" || arg == "1"); return "ok"; }
  if (verb == "monitor") { setMonitor(arg == "on" || arg == "1"); return "ok"; }
  if (verb == "name") { setUserName(arg); return "ok"; }
  if (verb == "bitrate")
  {
    const int k = atoi(arg.c_str());
    if (Settings::nearestOpusKbps(k) != k) return "error: the bitrate is one of 64 96 128 160 256 510";
    setOpusKbps(k);
    return "ok";
  }
  if (verb == "codec")
  {
    if (arg != "opus" && arg != "flac") return "error: codec opus or codec flac";
    setRoomCodec(arg);
    return "ok";
  }
  if (verb == "follow") { setFollowHostTempo(arg == "on" || arg == "1"); return "ok"; }
  if (verb == "vote")
  {
    // what the editor's BPM and BPI boxes do
    const size_t s2 = arg.find(' ');
    const std::string what = arg.substr(0, s2);
    const int value = s2 == std::string::npos ? 0 : atoi(arg.c_str() + s2 + 1);
    if ((what != "bpm" && what != "bpi") || value <= 0) return "error: vote bpm N, or vote bpi N";
    vote(what, value);
    return "ok";
  }
  if (verb == "nextinterval")
  {
    Snapshot s;
    getSnapshot(s);
    return s.nextIntervalPpq < 0 ? std::string() : nextIntervalText(s.hostChip, s.nextIntervalPpq, s.timeSigNum, s.timeSigDen);
  }
  if (verb == "syncnote")
  {
    Snapshot s;
    getSnapshot(s);
    return s.syncNote;
  }
  if (verb == "synctip")
  {
    Snapshot s;
    getSnapshot(s);
    return syncTooltip(s.hostLabel, s.linkState, s.linkPeers);
  }
  if (verb == "metronome")
  {
    const Settings st = getSettings();
    setMetronome(st.metronomeVolume, !(arg == "on" || arg == "1"));
    return "ok";
  }
  if (verb == "live")
  {
    // live on|off (the room's), live buffer N, live status
    if (arg == "on" || arg == "off")
    {
      setRoomLive(arg == "on");
      return "ok";
    }
    if (arg.compare(0, 6, "buffer") == 0)
    {
      char *end = nullptr;
      const std::string v = trim(arg.substr(6));
      const long ms = strtol(v.c_str(), &end, 10);
      if (v.empty() || !end || *end || ms < Settings::MIN_LIVE_BUFFER_MS || ms > Settings::MAX_LIVE_BUFFER_MS)
        return "error: live buffer N, N from " + std::to_string(Settings::MIN_LIVE_BUFFER_MS) + " to " +
               std::to_string(Settings::MAX_LIVE_BUFFER_MS) + " (ms)";
      setLiveBufferMs((int)ms);
      return "ok";
    }
    if (arg != "status") return "error: live on, live off, live buffer N or live status";
    Snapshot s;
    getSnapshot(s);
    char buf[1024];
    snprintf(buf, sizeof(buf), "live=%s offered=%d running=%d maysend=%d sending=%d buffer=%d inuse=%d hostblock=%d serverrtt=%.3f "
             "kbps=%d frame=%d sent=%llu received=%llu peers=%d\n",
             s.liveRoomOn ? "on" : "off", s.liveOffered ? 1 : 0, s.liveRunning ? 1 : 0, s.liveMaySend ? 1 : 0, s.liveSending ? 1 : 0,
             s.liveBufferMs, s.liveBufferInUseMs, s.liveHostBlock, s.liveServerRttMs, s.liveKbps, s.liveFrame, s.liveSent, s.liveReceived, (int)s.livePeers.size());
    std::string out = buf;
    for (const LivePeerView &p : s.livePeers)
    {
      snprintf(buf, sizeof(buf), "peer=%s shown=%s slot=%d bound=%d streaming=%d healthy=%d live=%d delay=%.3f rtt=%.3f bufferms=%.3f "
               "codec=%.3f loss=%.3f dropouts=%d underruns=%d overruns=%d dropped=%d resent=%d\n",
               p.name.c_str(), p.displayName.c_str(), p.slot, p.bound ? 1 : 0, p.streaming ? 1 : 0, p.healthy ? 1 : 0, p.live ? 1 : 0,
               p.delayMs, p.rttMs, p.bufferMs, p.codecMs, p.lossPct, p.dropouts(), p.underruns, p.overruns, p.dropped, p.resent);
      out += buf;
    }
    return out;
  }
  if (verb == "status")
  {
    Snapshot s;
    getSnapshot(s);
    char buf[1536];
    int n = snprintf(buf, sizeof(buf), "state=%s server=%s user=%s bpm=%.1f bpi=%d pos=%d len=%d codec=%s upload=%s kbps=%d users=%d hosting=%d port=%d public=%s local=%s license=%d audio=%d",
                           state_name(s.state), s.server.c_str(), s.userName.c_str(), s.bpm, s.bpi, s.intervalPos, s.intervalLength,
                           s.roomCodec.empty() ? "-" : s.roomCodec.c_str(), s.uploadCodec.empty() ? "-" : s.uploadCodec.c_str(),
                           s.sendKbps, (int)s.users.size(), s.hosting ? 1 : 0, s.hostPort,
                           s.hostPublicAddress.empty() ? "-" : s.hostPublicAddress.c_str(),
                           s.hostLocalAddress.empty() ? "-" : s.hostLocalAddress.c_str(), s.licensePending ? 1 : 0, s.audioRunning ? 1 : 0);
    if (n > 0 && n < (int)sizeof(buf))
      n += snprintf(buf + n, sizeof(buf) - n, " follow=%d hosttempo=%.3f bars=%s barerr=%.3f barfixes=%d tempovotes=%d ownkbps=%d roomkbps=%d",
                    s.followTempo ? 1 : 0, s.hostTempo, bar_state_name(s.barState), s.barErrorMs, s.barFixes, s.tempoVotes,
                    s.opusKbps, s.roomOpusKbps);
    if (n > 0 && n < (int)sizeof(buf))
      snprintf(buf + n, sizeof(buf) - n, " link=%s peers=%d linktempo=%.3f leads=%d linkaligned=%d linkerr=%.3f latency=%.3f "
               "jumps=%d steps=%d restores=%d",
               link_state_name(s.linkState), s.linkPeers, s.linkTempo, s.linkLeads ? 1 : 0, s.linkAligned ? 1 : 0, s.linkErrorMs,
               s.linkLatencyMs, s.linkJumps, s.linkSteps, s.linkRestores);
    std::string out = buf;
    out += " lookedup=" + (s.hostLookedUpAddress.empty() ? std::string(s.hostLookupPending ? "?" : "-") : s.hostLookedUpAddress);
    out += std::string(" joinedfrominternet=") + (s.hostJoinedFromInternet ? "1" : "0");
    return out;
  }
  if (verb == "users")
  {
    Snapshot s;
    getSnapshot(s);
    std::string out;
    for (auto &u : s.users)
      for (auto &c : u.channels)
      {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s/%d %s vol=%.3f pan=%.3f mute=%d solo=%d peak=%.4f shown=%s\n", u.name.c_str(), c.index,
                 c.name.c_str(), c.volume, c.pan, c.mute ? 1 : 0, c.solo ? 1 : 0, std::max(c.peak[0], c.peak[1]), u.displayName.c_str());
        out += buf;
      }
    return out;
  }
  if (verb == "chatlog")
  {
    std::vector<ChatLine> lines;
    getChat(lines, 200);
    std::string out;
    for (auto &l : lines) out += (l.who.empty() ? "" : "<" + l.who + "> ") + l.text + "\n";
    return out;
  }
  return "unknown command: " + verb;
}

} // namespace ninjam_vst3
