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
  can switch the room's codec with !codec (Opus or FLAC allowed), up to 8 users,
  and the router is asked to forward the port (UPnP-IGD or NAT-PMP).

  Ports 2049 to 2058 are tried in turn. start() and stop() block (stop waits for
  the router to remove the forward), so call them off the UI and audio threads.

*/

#ifndef _NINJAM_VST3_HOSTSERVER_H_
#define _NINJAM_VST3_HOSTSERVER_H_

#include <mutex>
#include <string>
#include <vector>

namespace ninjam_vst3 {

class HostServer
{
public:
  static HostServer &instance();

  enum { FIRST_PORT = 2049, PORTS_TO_TRY = 10 };

  // true when started (or already running for this owner)
  bool start(const void *owner, std::string *error);
  void stop(const void *owner);

  struct State
  {
    const void *owner = nullptr; // null when no server runs
    int port = 0;
    std::string publicAddress;   // host:port others use, once the router forwards the port
    std::string portmapNote;     // why there is no public address (the last thing the port mapper said)
    bool portmapPending = false; // still asking the router
    std::string localAddress;    // this machine's address in the local network, with the port
    int users = 0;
  };
  State state();

  // log lines from the server that are worth showing (port mapping, errors), after serial
  std::vector<std::string> messagesSince(unsigned long long *serial);

  // parses a server log line; public for the tests
  static void parseLogLine(const std::string &line, State &st, bool *show);

private:
  HostServer() {}
  static void onLog(void *ctx, const char *text);
  static std::string writeConfig(int port);

  std::mutex m_startStop; // held while starting or stopping
  std::mutex m_mutex;     // guards everything below
  State m_state;
  std::vector<std::string> m_messages;
  unsigned long long m_serial = 0;
};

// the address of this machine in its local network ("192.168.1.20"), or "" if not known
std::string localNetworkAddress();

} // namespace ninjam_vst3

#endif
