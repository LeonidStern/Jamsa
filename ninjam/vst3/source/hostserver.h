/*
    NINJAM VST3 - hostserver.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  The built-in server (ninjamsrv_embedded): one per process, so one plug-in
  instance at a time owns it. It is set up like JamTaba's private server:
  anyone can join without an account, a single vote changes BPM/BPI, any player
  can switch the room's codec with !codec (Opus or FLAC allowed) and its Opus bitrate
  with !bitrate (at first the host's own), up to 8 users,
  and the router is asked to forward the port (UPnP-IGD or NAT-PMP).

  While it runs, a "what's my IP" service on the internet is asked for this
  computer's public address, for when the router doesn't say (no UPnP or NAT-PMP,
  or a port forwarded by hand): JAMSA_WHATSMYIP=host:port/path points it elsewhere
  (the tests), and JAMSA_WHATSMYIP=off turns it off.

  Ports 2049 to 2058 are tried in turn. start() and stop() block (stop waits for
  the router to remove the forward), so call them off the UI and audio threads.

*/

#ifndef _NINJAM_VST3_HOSTSERVER_H_
#define _NINJAM_VST3_HOSTSERVER_H_

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ninjam_vst3 {

class HostServer
{
public:
  static HostServer &instance();

  enum { FIRST_PORT = 2049, PORTS_TO_TRY = 10 };

  // true when started (or already running for this owner); the room's Opus bitrate starts at opusKbps
  bool start(const void *owner, int opusKbps, std::string *error);
  void stop(const void *owner);

  struct State
  {
    const void *owner = nullptr; // null when no server runs
    int port = 0;
    std::string publicAddress;   // host:port others use, once the router forwards the port
    std::string portmapNote;     // why there is no public address (the last thing the port mapper said)
    bool portmapPending = false; // still asking the router
    std::string localAddress;    // this machine's address in the local network, with the port
    std::string lookedUpAddress; // the public address with the port, from a "what's my IP" service: where
                                 // others join if the port is forwarded (the router may not say it is)
    bool lookupPending = false;  // still asking that service
    int users = 0;
  };
  State state();

  // log lines from the server that are worth showing (port mapping, errors), after serial
  std::vector<std::string> messagesSince(unsigned long long *serial);

  // parses a server log line; public for the tests
  static void parseLogLine(const std::string &line, State &st, bool *show);
  // the IPv4 address in a "what's my IP" service's HTTP reply ("HTTP/1.1 200 OK ... 89.114.98.123\n")
  static bool parseWhatsMyIpReply(const std::string &reply, std::string *ip);

private:
  HostServer() {}
  ~HostServer();
  void lookUpPublicAddress(int port);
  void stopLookup();
  static void onLog(void *ctx, const char *text);
  static std::string writeConfig(int port, int opusKbps);

  std::mutex m_startStop; // held while starting or stopping
  std::mutex m_mutex;     // guards everything below
  State m_state;
  std::vector<std::string> m_messages;
  unsigned long long m_serial = 0;
  std::thread m_lookup; // asks the "what's my IP" service; joined by stop()
  std::atomic<bool> m_lookupStop { false };
};

// the address of this machine in its local network ("192.168.1.20"), or "" if not known
std::string localNetworkAddress();

} // namespace ninjam_vst3

#endif
