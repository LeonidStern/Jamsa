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
};

struct RemoteUserView
{
  std::string name;        // as the server names the user (name@address)
  std::string displayName; // without the @address
  std::vector<RemoteChannelView> channels;
};

enum class ConnState { Disconnected, Connecting, Connected, Failed };

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
    HostPosition position;
  };
  void setupProcessing(double sampleRate, int maxBlockSize); // not while processing
  void process(float **in, int numIn, float **out, int numOut, int frames, const HostTiming *timing = nullptr);

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
  void setEditorSize(int width, int height);
  void setFollowHostTempo(bool on); // Sync, and with it Ableton Link
  void setHostName(const std::string &name); // the host's name, for the editor

  enum RemoteField { RemoteVolume, RemotePan, RemoteMute, RemoteSolo };
  void setRemote(const std::string &user, int channel, RemoteField field, float value);

  // Text commands, for tests and automation (see plugin.cpp); returns the reply:
  //   connect host:port [name], disconnect, name X, host on|off, chat text (sent as typed),
  //   license accept|decline, transmit on|off, monitor on|off, metronome on|off,
  //   bitrate 64|96|128|160|256|510, codec opus|flac, follow on|off (Sync, with Link), vote bpm|bpi N,
  //   status, users, chatlog, syncnote, nextinterval, synctip (the Sync button's tooltip)
  std::string command(const std::string &line);

  static std::string remoteKey(const std::string &user, int channel); // user without @address
  static std::string displayName(const std::string &user); // "alice@1.2.3.4" is "alice", "alice@1.2.3.4.2" "alice.2"
  // the name a new instance starts with: the first name of the computer account's full name
  // ("Leo Stern" is "Leo"), else its login name; "" if neither is known
  static std::string defaultUserName();
  static std::string firstName(const std::string &fullName); // "Leo Stern" is "Leo"

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
  void updateConnection();
  void updateUsers(Snapshot &snap);
  void pollHosting(Snapshot &snap);
  void checkCanSend(bool canSend);
  void followHostTempo();
  void updateLink();
  void releaseLink(); // stop leading Link, between two blocks
  void publish();
  int findUser(const std::string &name) const;

  static int licenseCallback(void *user, const char *text);
  static void chatCallback(void *user, NJClient *inst, const char **parms, int nparms);

  std::unique_ptr<NJClient> m_client;
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
  int m_maxBlock = 0;
  std::vector<float> m_bufL, m_bufR, m_spare;
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
};

} // namespace ninjam_vst3

#endif
