/*
    Jamsa Live - liveclient.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Live mode for one plug-in instance: the players in a room hear each other
  in real time, through the NINJAM server the instance is connected to.

  Our input goes out as one Opus stream (AOO, see README.md), sent once to
  the server, which copies it to the other Live players in the room. Each
  of them comes back on a slot: a fixed pool of MAX_PEERS receivers, each
  bound to one peer. The engine mixes a slot's audio (its "stem") instead of
  that player's intervals while the slot is healthy.

  Threads:
  - the engine's network thread (the "control thread") calls start(),
    update(), setPeers(), setJitterMs(), stop(), poll() and stats();
  - the audio thread calls processBlock() once per host block, then reads
    slotOf(), slotHealthy() and stem(); none of these waits, allocates or
    logs (prepare() comes from setupProcessing, not while processing);
  - the Client runs two threads of its own while started: one receives
    from the server, the other sends (raised priority, woken by the audio
    thread). Neither ever touches the engine.

  This header includes nothing from AOO or the system's socket headers, so
  the engine can include it next to WDL's jnetlib (winsock 1 on Windows).

*/

#ifndef JAMSA_LIVE_CLIENT_H
#define JAMSA_LIVE_CLIENT_H

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace jamsa_live {

// What the server said in MESSAGE_SERVER_LIVE; the engine fills it in.
struct Config
{
  uint32_t myId = 0;       // your_peer_id
  uint64_t key = 0;        // the connection's secret, in every datagram we send
  uint32_t serverIp = 0;   // IPv4, network byte order: the TCP connection's remote address
  uint16_t udpPort = 0;    // resolved (the TCP port when the message says 0)
  int kbps = 128;          // the room's Live Opus bitrate (32..510)
  int frameSamples = 240;  // Opus frame at 48 kHz: 120, 240 or 480
  bool udpOpen = false;    // the server takes Live UDP
  bool roomOn = false;     // the room's Live flag
  bool maySend = false;    // we are one of the room's first LiveMaxPeers players
};

// another Live player in the room
struct Peer
{
  uint32_t id = 0;
  std::string name;
  bool bound = false;      // the server has a UDP address for it
};

struct PeerStats
{
  uint32_t id = 0;
  std::string name;
  int slot = -1;           // -1: no free slot (more than MAX_PEERS players)
  bool streaming = false;  // we hear its stream
  bool healthy = false;    // streaming for 1 s or more, with few dropouts lately
  double rttMs = 0;        // round trip to that player through the server (0 = not known yet)
  double bufferMs = 0;     // our jitter buffer for it
  double codecMs = 0;      // its encoder (frame, lookahead) plus our decoder and resampler
  double oneWayMs = 0;     // how late we hear it, without either audio interface
  double lossPct = 0;      // its packets lost on the way, resent or not, in percent
  int underruns = 0;       // our buffer ran dry
  int overruns = 0;        // our buffer overflowed: blocks thrown away
  int dropped = 0;         // blocks lost for good (Opus concealed them)
  int resent = 0;          // blocks lost and then resent in time
};

// for /live status and the tests
struct Counters
{
  uint64_t sent = 0;          // datagrams sent to the server
  uint64_t received = 0;      // datagrams received from it
  uint64_t accepted = 0;      // handed to AOO
  uint64_t droppedBad = 0;    // not our protocol, wrong size
  uint64_t droppedPeer = 0;   // from a player without a slot
  uint64_t droppedFilter = 0; // an AOO message we don't take (IDs, kinds, formats)
  uint64_t droppedIdle = 0;   // for a slot while processBlock() wasn't being called
  uint64_t helloAcks = 0;
  bool sendThreadRealtime = false; // the send thread got a raised priority
};

class Client
{
public:
  static constexpr int MAX_PEERS = 8;

  Client();
  ~Client();
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;

  // control thread
  // Opens the UDP socket (connected to the server), starts the receive and
  // send threads and the HELLOs. Fails (with a reason) without udpOpen, an
  // id or an address, or when the socket can't be opened.
  bool start(const Config &c, std::string *error);
  // room on/off, bitrate, frame size, maySend; a new server, id or key
  // restarts; udpOpen false stops. Nothing before start().
  void update(const Config &c);
  // the room's other Live players: keeps the slots of those still there,
  // gives free slots to new ones (in this order), frees the others
  void setPeers(const std::vector<Peer> &peers);
  void setJitterMs(int ms); // our jitter buffer, 5..200 ms (default 20)
  void stop();
  bool running() const;
  void poll(); // AOO's events -> statistics and slot health; every ~30 ms
  std::vector<PeerStats> stats() const;
  double serverRttMs() const; // from HELLO/HELLO_ACK; 0 until the first answer
  Counters counters() const;

  // audio thread
  // Sizes the buffers; from setupProcessing. Not while processBlock() runs
  // (it would skip a block rather than wait).
  void prepare(double sampleRate, int maxBlock);
  // Once per host block: sends our input (while 'send' and inL; otherwise
  // our stream stops, rather than send silence), and renders every slot's
  // stem for this block. inR may be null (mono).
  void processBlock(const float *inL, const float *inR, int frames, bool send);
  int slotOf(uint32_t peerId) const;   // -1 if none
  bool slotHealthy(int slot) const;
  // this block's audio of a slot, 'frames' long; silence when idle
  const float *stem(int slot, int channel) const;

  struct Impl;

private:
  std::unique_ptr<Impl> m_impl;
};

} // namespace jamsa_live

#endif
