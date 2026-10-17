/*
    NINJAM VST3 - engine.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  One plug-in instance's NINJAM client.

  Threads:
  - the audio thread calls process(), which runs NJClient::AudioProc;
  - the engine's own network thread is the only one that touches the NJClient
    otherwise: it runs NJClient::Run(), carries out what the UI asked for
    (a queue of commands), and publishes a Snapshot of the state;
  - the UI (and the host, for the plug-in's state) only reads snapshots and
    queues commands, so it never waits for the network.

  The network thread runs from start() to stop(), editor open or not.

  The audio path is: input * input volume -> NJClient (sent as our channel,
  and monitored if monitoring is on) -> output = the other players + metronome
  (+ our input when monitoring). process() is where to add more processing
  later, such as a looper. The metronome is the plug-in's own (click.h), on the
  beats of NJClient's intervals; NJClient's is muted.

  Following the host (hostsync.h): while "Sync" is on,
  process() lines the intervals up with the host's bars, and the network
  thread votes the room to the host's tempo when it changes.

  Ableton Link (linksync.h): while "Sync" is on in a room the plug-in is in
  Link, and one instance in the program leads it: process() puts the room's
  tempo and the intervals' beat on Link, so Live (with its Link on) follows
  the room, and while Link has peers it leaves the intervals where they are
  (no BarSync). The network thread still votes the room to the host's tempo
  when it changes, and puts the room's tempo back on Link when the room
  doesn't take it or another app set it (LinkTempoKeeper).

  Live mode (ninjam/live, liveclient.h): in a room whose server has Live, while
  the room's Live is on (!live on), the Jamsa players hear each other in real
  time. Our input goes to the server in UDP datagrams, and each other player's
  real-time audio (a "slot" of the Live client) takes the place of their
  intervals in NJClient's mix, from an interval's start on, while it plays
  well; they go back to intervals at an interval's start when it doesn't (at
  once when the room turns Live off). Intervals keep flowing both ways, for
  everyone else. The network thread runs the Live client (updateLive()); the
  audio thread feeds it and reads the slots' audio, without waiting. The
  jitter buffer is the one set (/live buffer N), or two of the host's blocks
  and 5 ms when that is more (liveBufferFor()).

*/

#ifndef _NINJAM_VST3_ENGINE_H_
#define _NINJAM_VST3_ENGINE_H_

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "click.h"
#include "hostsync.h"
#include "linksync.h"

class NJClient;

namespace jamsa_live { class Client; }

namespace ninjam_vst3 {

struct ChatLine
{
  enum Kind { System, Server, Message, Action, Private, Topic, Error };
  Kind kind = System;
  std::string who; // display name, without the @address
  std::string text;
};

struct RemoteChannelView
{
  int index = 0; // the channel's index at its user
  std::string name;
  float volume = 1.0f, pan = 0.0f;
  bool mute = false, solo = false;
  float peak[2] = { 0, 0 };
  int playerOut = 0; // the plug-in's extra output that plays it alone (1 to Engine::NUM_PLAYER_OUTS), 0 for none
  bool live = false; // you hear this channel live (Live mode), not its intervals; its name ends in " · live"
};

struct RemoteUserView
{
  std::string name;        // as the server names the user (name@address)
  std::string displayName; // without the @address
  std::vector<RemoteChannelView> channels;
};

enum class ConnState { Disconnected, Connecting, Connected, Failed };

// another player in Live mode, as the Live client sees them
struct LivePeerView
{
  std::string name;        // as the server names the user (name@address)
  std::string displayName; // without the @address
  int slot = -1;           // the Live client's receiver for them, -1 for none (more than it has)
  bool bound = false;      // the server has their UDP address
  bool streaming = false;  // their real-time audio comes in
  bool healthy = false;    // and has for a second, with few dropouts
  bool live = false;       // you hear them live now (not their intervals)
  double delayMs = 0;      // how late you hear them, without either audio interface (estimate)
  double rttMs = 0, bufferMs = 0, codecMs = 0, lossPct = 0;
  int underruns = 0, overruns = 0, dropped = 0, resent = 0;
  int dropouts() const { return underruns + overruns + dropped; }
};

struct Snapshot
{
  ConnState state = ConnState::Disconnected;
  std::string status; // one line for the UI
  std::string server; // connected or connecting to
  std::string userName; // the name the server gave us
  float bpm = 0;  // as the server set them
  int bpi = 0;
  int intervalPos = 0, intervalLength = 0, intervalBeats = 0; // the interval playing now
  bool audioRunning = false; // the host called process() in the last second
  std::string roomCodec;     // "Opus", "FLAC" or "Vorbis" while connected
  std::string uploadCodec;   // what we send ('OPUS' or 'FLAC'), empty when the room has neither
  bool canSend = true;       // false in a Vorbis room (or on a server without Opus): nothing is sent
  int opusKbps = 0;          // your own Opus bitrate (Settings::opusKbps)
  int roomOpusKbps = 0;      // the room's, while connected to a server that sets one; 0: none
  int sendKbps = 0;          // what you send in Opus rooms: the room's, else your own (from the next interval)
  float localPeak[2] = { 0, 0 };
  bool sendPaused = false;   // the host is stopped, so nothing is sent (transmit stays on)

  // Live mode
  bool liveOffered = false;  // the server has Live, and its UDP port is open
  bool liveRoomOn = false;   // the room's Live is on
  bool liveRunning = false;  // our Live client runs (talks to the server over UDP)
  bool liveMaySend = false;  // the server takes our real-time audio (one of the room's first Live players)
  bool liveSending = false;  // we send it: may send, transmit on, the host not stopped
  int liveBufferMs = 0;      // the jitter buffer asked for (Settings::liveBufferMs)
  int liveBufferInUseMs = 0; // the one Live uses: more with the host's long blocks (Engine::liveBufferFor())
  int liveHostBlock = 0;     // the host's longest block lately (samples)
  double liveServerRttMs = 0; // the round trip to the server's UDP port, 0 until it answers
  int liveKbps = 0, liveFrame = 0; // the room's Live Opus bitrate and frame (samples at 48 kHz)
  unsigned long long liveSent = 0, liveReceived = 0; // datagrams
  std::vector<LivePeerView> livePeers;

  std::vector<RemoteUserView> users;
  unsigned int usersRevision = 0; // changes when users or channels come or go (not for levels)

  bool licensePending = false;
  std::string licenseText;

  // the built-in server
  // following the host (hostsync.h)
  bool followTempo = true;   // the toggle
  std::string hostLabel;     // "Live", or the host's name
  std::string hostChip;      // the same, short: "Live", or "Host" for a long name
  double nextIntervalPpq = -1; // where the next interval starts in the host (quarter notes), -1 when not in line
  int timeSigNum = 0, timeSigDen = 0; // the host's time signature, 0 when it doesn't say
  double hostTempo = 0;      // 0 when the host doesn't say (or doesn't run the audio)
  BarSync::State barState = BarSync::Off;
  double barErrorMs = 0;     // where the host's interval is, minus ours
  int barFixes = 0;          // intervals made shorter or longer to line up
  int tempoVotes = 0;        // votes for the host's tempo
  std::string syncNote;      // the host's tempo isn't the room's (see hostTempoNote)

  // Ableton Link (linksync.h), while Sync is on in a room
  LinkState linkState = LinkState::Off;
  int linkPeers = 0;         // the other Link apps (Live, with its Link on)
  double linkTempo = 0;      // the Link session's, 0 when not in Link
  bool linkLeads = false;    // this instance leads Link, which has peers: Live's bars follow the intervals
  bool linkAligned = false;  // the intervals start on Link's beat
  double linkErrorMs = 0;    // the interval's beat minus Link's phase
  double linkLatencyMs = 0;  // when the host's blocks are heard, after the plug-in's guess (Calibrator)
  int linkJumps = 0, linkSteps = 0, linkRestores = 0;

  bool hosting = false;      // this instance hosts it
  bool hostBusy = false;     // starting or stopping
  int hostPort = 0;
  std::string hostPublicAddress, hostLocalAddress, hostNote;
  bool hostPortmapPending = false;
  std::string hostLookedUpAddress; // from a "what's my IP" service, for when the router doesn't say
  bool hostLookupPending = false;
  bool hostJoinedFromInternet = false; // so the port is open, even if the router didn't say
  int hostUsers = 0;
  int hostedElsewherePort = 0; // another instance in this program hosts one

  unsigned long long chatRevision = 0;
  unsigned int settingsRevision = 0;
};

// what the plug-in saves with the host's project
struct Settings
{
  // the Opus bitrates (kbps) to pick from; 510 is libopus's stereo maximum
  static constexpr int OPUS_KBPS[] = { 64, 96, 128, 160, 256, 510 };
  static constexpr int NUM_OPUS_KBPS = 6, DEFAULT_OPUS_KBPS = 510; // the best Opus there is, by default
  static int nearestOpusKbps(int kbps);

  std::string userName;
  std::string server;
  float inputVolume = 1.0f;
  float metronomeVolume = 0.5f;
  bool metronomeMute = true; // off until you turn it on
  bool monitor = true;
  bool transmit = true;
  int opusKbps = DEFAULT_OPUS_KBPS; // what we send in Opus rooms; FLAC is lossless and ignores it
  bool followHostTempo = true; // Sync: follow the host's tempo (and line up with its bars)
  // Live mode's jitter buffer (ms): lower is less late, higher has fewer dropouts
  static constexpr int MIN_LIVE_BUFFER_MS = 5, MAX_LIVE_BUFFER_MS = 200, DEFAULT_LIVE_BUFFER_MS = 20;
  int liveBufferMs = DEFAULT_LIVE_BUFFER_MS;
  int editorWidth = 0, editorHeight = 0; // the editor's size, 0 for the default

  struct Remote
  {
    float volume = 1.0f, pan = 0.0f;
    bool mute = false;
  };
  std::map<std::string, Remote> remote; // by remoteKey()

  std::string serialize() const;
  bool deserialize(const std::string &data); // false if it isn't a saved state
};

class Engine
{
public:
  Engine();
  ~Engine();

  void start(); // starts the network thread
  void stop();  // disconnects, stops hosting, joins the network thread

  // audio thread
  struct HostTiming // from the host's ProcessContext
  {
    bool offline = false; // rendering faster than real time: the intervals are left alone
    bool transport = false; // the host passed its transport (position.playing means something)
    HostPosition position;
  };
  // Besides the main output (everyone, your monitoring and the click), the plug-in has this many
  // stereo outputs, "Player 1" to "Player 8", that each play one player alone. NJClient gives each
  // player who joins the least used one, so a ninth player shares one.
  static constexpr int NUM_PLAYER_OUTS = 8;
  struct PlayerOuts
  {
    float **bus[NUM_PLAYER_OUTS] = {}; // nullptr for an output the host doesn't use
    int channels[NUM_PLAYER_OUTS] = {};
  };
  void setupProcessing(double sampleRate, int maxBlockSize); // not while processing
  // While the host says it is stopped, the plug-in sends silence, and nothing from the next
  // interval on; your monitoring goes on.
  void process(float **in, int numIn, float **out, int numOut, int frames, const HostTiming *timing = nullptr,
               const PlayerOuts *playerOuts = nullptr);

  // any thread
  void getSnapshot(Snapshot &out) const;
  unsigned long long getChat(std::vector<ChatLine> &out, size_t maxLines) const; // returns the revision
  Settings getSettings() const;
  void setSettings(const Settings &s); // a saved state was loaded

  void connect(const std::string &server, const std::string &userName);
  void disconnect();
  void sendChat(const std::string &text);
  void vote(const std::string &what, int value); // "!vote bpm 120" or "!vote bpi 16", sent as chat
  void answerLicense(bool accept);
  void setHosting(bool on);

  void setUserName(const std::string &name);
  void setServerText(const std::string &server);
  void setInputVolume(float v);
  void setTransmit(bool on);
  void setMonitor(bool on);
  void setMetronome(float volume, bool mute);
  // One of Settings::OPUS_KBPS: your own, and in a room whose server sets one for everyone
  // (see mpb.h), the room's too (!bitrate N). It applies from the next interval.
  void setOpusKbps(int kbps);
  void setRoomCodec(const std::string &codec); // "opus" or "flac": the room's (!codec), while connected
  void setRoomLive(bool on); // the room's Live mode (!live on|off), while connected
  void setLiveBufferMs(int ms); // Live's jitter buffer, MIN_LIVE_BUFFER_MS to MAX_LIVE_BUFFER_MS
  void setEditorSize(int width, int height);
  void setFollowHostTempo(bool on); // Sync, and with it Ableton Link
  void setHostName(const std::string &name); // the host's name, for the editor

  enum RemoteField { RemoteVolume, RemotePan, RemoteMute, RemoteSolo };
  void setRemote(const std::string &user, int channel, RemoteField field, float value);

  // Text commands, for tests and automation (see plugin.cpp); returns the reply:
  //   connect host:port [name], disconnect, name X, host on|off, chat text (sent as typed),
  //   license accept|decline, transmit on|off, monitor on|off, metronome on|off,
  //   bitrate 64|96|128|160|256|510, codec opus|flac, follow on|off (Sync, with Link), vote bpm|bpi N,
  //   status, users, chatlog, syncnote, nextinterval, synctip (the Sync button's tooltip),
  //   live on|off (the room's, !live), live buffer N (ms), live status (key=value lines: the room,
  //   then one line per other Live player)
  std::string command(const std::string &line);

  static std::string remoteKey(const std::string &user, int channel); // user without @address
  static std::string displayName(const std::string &user); // "alice@1.2.3.4" is "alice", "alice@1.2.3.4.2" "alice.2"
  // the name a new instance starts with: the first name of the computer account's full name
  // ("Leo Stern" is "Leo"), else its login name; "" if neither is known
  static std::string defaultUserName();
  static std::string firstName(const std::string &fullName); // "Leo Stern" is "Leo"
  // Live's jitter buffer for the one asked for: a player's audio comes a host block at a time
  // and plays a block at a time, so with long blocks it needs two of them and a little more
  // (or it runs dry or overflows every few seconds)
  static int liveBufferFor(int askedMs, int hostBlock, int sampleRate);

private:
  void threadProc();
  void post(std::function<void()> fn);
  void runCommands();
  void addChat(ChatLine::Kind kind, const std::string &who, const std::string &text);

  // network thread
  void doConnect(const std::string &server, const std::string &user);
  void doDisconnect(const char *why);
  void clientConnect(const std::string &server, const std::string &login);
  void clientDisconnect();
  void doSetHosting(bool on);
  void applyLocalSettings();
  int sendKbps();       // the Opus bitrate to send: the room's, else your own
  void applySendKbps(); // sets the local channel's bitrate to it
  void applyBroadcast(); // sends while transmit is on and the host isn't stopped
  void updateConnection();
  void updateUsers(Snapshot &snap);
  void pollHosting(Snapshot &snap);
  void checkCanSend(bool canSend);
  void followHostTempo();
  void updateLink();
  void releaseLink(); // stop leading Link, between two blocks
  void updateLive();  // starts, updates and stops the Live client from what the server says
  void applyLiveBuffer(); // the jitter buffer in use, for the setting and the host's blocks
  void stopLive();
  void liveChatCommand(const std::string &arg); // /live, /live buffer N
  void updateLiveView(Snapshot &snap);
  void publish();
  int findUser(const std::string &name) const;

  static int licenseCallback(void *user, const char *text);
  static void chatCallback(void *user, NJClient *inst, const char **parms, int nparms);
  static bool liveAudio(void *user, int slot, int offset, int len, const float **left, const float **right);

  std::unique_ptr<NJClient> m_client;
  std::unique_ptr<jamsa_live::Client> m_live;
  // NJClient's Connect() and Disconnect() free what its AudioProc() uses, so they hold this,
  // and process() skips the block (plays silence) when it can't get it
  std::mutex m_audioMutex;

  std::thread m_thread;
  std::atomic<bool> m_quit { false };
  bool m_running = false;
  std::mutex m_cmdMutex;
  std::condition_variable m_cmdCv;
  std::deque<std::function<void()>> m_commands;

  mutable std::mutex m_snapMutex;
  Snapshot m_snap;

  mutable std::mutex m_chatMutex;
  std::deque<ChatLine> m_chat;
  unsigned long long m_chatRevision = 0;

  mutable std::mutex m_settingsMutex;
  Settings m_settings;
  unsigned int m_settingsRevision = 0;
  std::string m_hostName; // under m_settingsMutex

  std::atomic<int> m_licenseAnswer { 0 }; // 1 accept, -1 decline
  std::atomic<bool> m_licenseWaiting { false };

  // audio thread
  std::atomic<float> m_inputGain { 1.0f };
  std::atomic<float> m_clickVolume { 0.5f }; // 0 when muted
  Click m_click;
  std::atomic<int> m_sampleRate { 48000 };
  std::atomic<unsigned int> m_processCalls { 0 };
  std::atomic<int> m_hostFrames { 0 }; // the last block's length (for Live's jitter buffer)
  int m_maxBlock = 0;
  std::vector<float> m_bufL, m_bufR, m_spare;
  std::vector<float> m_monL, m_monR;      // the input, for your monitoring
  std::vector<std::vector<float>> m_mix;  // NJClient's outputs: the main pair, then a pair per player output
  std::atomic<bool> m_monitor { true }, m_anySolo { false };
  std::atomic<bool> m_hostStopped { false }; // the host said it is stopped
  std::atomic<bool> m_offline { false };     // the host renders faster than real time
  std::atomic<bool> m_transmit { true };
  // Live: the client runs and the room's Live is on, so process() feeds it and NJClient plays the
  // slots of the players it plays live (else their intervals, at once)
  std::atomic<bool> m_liveOn { false };
  std::vector<float> m_liveL, m_liveR;  // the input after the input volume, for Live
  bool m_liveBlock = false;  // the Live client rendered the slots for the samples from m_liveStart
  int m_liveStart = 0;       // where in the host's block that is
  int m_pieceStart = 0;      // where in the host's block the AudioProc() running now starts
  BarSync m_barSync;
  std::atomic<bool> m_follow { true }, m_barReset { false };
  int64_t m_sampleTime = 0; // samples processed since setupProcessing()
  std::unique_ptr<LinkSync> m_link;
  std::atomic<bool> m_linkLeads { false }; // process() lines Link up with the intervals, and Link has peers
  // what the audio thread saw, for the network thread
  std::atomic<double> m_hostTempo { 0 }, m_barErrorMs { 0 };
  std::atomic<double> m_nextIntervalPpq { -1 };
  std::atomic<int> m_timeSig { 0 }; // numerator * 256 + denominator
  std::atomic<int> m_barState { BarSync::Off }, m_barFixes { 0 };

  // network thread only
  ConnState m_state = ConnState::Disconnected;
  std::string m_server, m_statusText;
  double m_connectTime = 0, m_connectedTime = 0;
  std::set<std::string> m_appliedRemote; // remote channels given their saved settings this session
  unsigned int m_usersRevision = 0;
  bool m_hosting = false, m_hostBusy = false;
  int m_hostPort = 0;
  unsigned long long m_hostLogSerial = 0;
  bool m_warnedCantSend = false; // told the user this room can't hear them
  double m_lastPublish = 0;
  unsigned int m_seenProcessCalls = 0;
  std::map<std::string, int> m_playerOutSaid; // the player output we last told about, by user name
  double m_lastAudio = -10;
  TempoFollower m_tempoFollower;
  double m_noteSince = -1; // since when the host's tempo isn't the room's
  // Link
  bool m_armPending = false; // joined a room, or Sync went on: vote for the host's tempo, unless Live follows Link
  LinkTempoKeeper m_linkKeeper;
  LinkState m_linkState = LinkState::Off;
  size_t m_linkPeers = 0;
  double m_linkTempo = 0, m_lastLinkPoll = 0;
  bool m_toldLinkHint = false, m_toldContested = false, m_toldLinkLeads = false;
  // Live
  int m_liveGen = 0;               // NJClient's Live message generation the peers come from
  double m_lastLivePoll = 0;
  double m_liveStartedAt = 0;      // when the client started (for "no answer from the server")
  double m_liveRetryAt = 0;        // when to try starting it again, after it failed
  bool m_toldLiveOffered = false, m_toldLiveNoAnswer = false, m_toldLiveError = false;
  struct LiveUser
  {
    int on = -1;               // the slot NJClient plays now, -1 for intervals
    bool want = false;         // we asked NJClient to play it live from the next interval
    int slot = -1;             // with this slot
    double healthySince = -1;  // since when the slot plays well, -1 when it doesn't
    double fellBackAt = -1;    // when they went back to intervals because the slot played badly
  };
  std::map<std::string, LiveUser> m_liveUsers; // by user name
  // what /live counts dropouts from: since the buffer last changed
  double m_liveCountSince = 0;
  int m_liveBufferApplied = 0;      // the jitter buffer the Live client has (ms)
  double m_liveBufferChangedAt = -1e9;
  int m_liveHostBlock = 0;          // the host's longest block in the last half minute
  double m_liveHostBlockAt = 0;     // when it was last that long
  std::map<std::string, int> m_liveDropoutsBase; // by user name
};

} // namespace ninjam_vst3

#endif
