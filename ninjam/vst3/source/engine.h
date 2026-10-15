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
  later, such as a looper.

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
  int opusKbps = 0;          // the Opus bitrate asked for (it applies from the next interval)
  float localPeak[2] = { 0, 0 };

  std::vector<RemoteUserView> users;
  unsigned int usersRevision = 0; // changes when users or channels come or go (not for levels)

  bool licensePending = false;
  std::string licenseText;

  // the built-in server
  bool hosting = false;      // this instance hosts it
  bool hostBusy = false;     // starting or stopping
  int hostPort = 0;
  std::string hostPublicAddress, hostLocalAddress, hostNote;
  bool hostPortmapPending = false;
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
  static constexpr int NUM_OPUS_KBPS = 6, DEFAULT_OPUS_KBPS = 128;
  static int nearestOpusKbps(int kbps);

  std::string userName;
  std::string server;
  float inputVolume = 1.0f;
  float metronomeVolume = 0.5f;
  bool metronomeMute = false;
  bool monitor = true;
  bool transmit = true;
  int opusKbps = DEFAULT_OPUS_KBPS; // what we send in Opus rooms; FLAC is lossless and ignores it
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
  void setupProcessing(double sampleRate, int maxBlockSize); // not while processing
  void process(float **in, int numIn, float **out, int numOut, int frames);

  // any thread
  void getSnapshot(Snapshot &out) const;
  unsigned long long getChat(std::vector<ChatLine> &out, size_t maxLines) const; // returns the revision
  Settings getSettings() const;
  void setSettings(const Settings &s); // a saved state was loaded

  void connect(const std::string &server, const std::string &userName);
  void disconnect();
  void sendChat(const std::string &text);
  void answerLicense(bool accept);
  void setHosting(bool on);

  void setUserName(const std::string &name);
  void setServerText(const std::string &server);
  void setInputVolume(float v);
  void setTransmit(bool on);
  void setMonitor(bool on);
  void setMetronome(float volume, bool mute);
  void setOpusKbps(int kbps); // one of Settings::OPUS_KBPS; it applies from the next interval
  void setEditorSize(int width, int height);

  enum RemoteField { RemoteVolume, RemotePan, RemoteMute, RemoteSolo };
  void setRemote(const std::string &user, int channel, RemoteField field, float value);

  // Text commands, for tests and automation (see plugin.cpp); returns the reply:
  //   connect host:port [name], disconnect, name X, host on|off, chat text (sent as typed),
  //   license accept|decline, transmit on|off, monitor on|off, metronome on|off,
  //   bitrate 64|96|128|160|256|510, status, users, chatlog
  std::string command(const std::string &line);

  static std::string remoteKey(const std::string &user, int channel); // user without @address
  static std::string displayName(const std::string &user);

private:
  void threadProc();
  void post(std::function<void()> fn);
  void runCommands();
  void addChat(ChatLine::Kind kind, const std::string &who, const std::string &text);

  // network thread
  void doConnect(const std::string &server, const std::string &user);
  void doDisconnect(const char *why);
  void doSetHosting(bool on);
  void applyLocalSettings();
  void updateConnection();
  void updateUsers(Snapshot &snap);
  void pollHosting(Snapshot &snap);
  void checkCanSend(bool canSend);
  void publish();
  int findUser(const std::string &name) const;

  static int licenseCallback(void *user, const char *text);
  static void chatCallback(void *user, NJClient *inst, const char **parms, int nparms);

  std::unique_ptr<NJClient> m_client;

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

  std::atomic<int> m_licenseAnswer { 0 }; // 1 accept, -1 decline
  std::atomic<bool> m_licenseWaiting { false };

  // audio thread
  std::atomic<float> m_inputGain { 1.0f };
  std::atomic<int> m_sampleRate { 48000 };
  std::atomic<unsigned int> m_processCalls { 0 };
  int m_maxBlock = 0;
  std::vector<float> m_bufL, m_bufR, m_spare;

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
};

} // namespace ninjam_vst3

#endif
