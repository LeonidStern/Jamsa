/*
    Jamsa Live - livecore.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  The parts of Live mode that don't touch a socket or start a thread, so
  that the tests can run them on a virtual clock and network:

  - wire: the UDP datagrams between a player and the server (README.md,
    "Protocol"): a 20-byte header from the player (who, the key, to whom), an
    8-byte one from the server (from whom), the AOO packet after it;

  - Core: one AooSource (ID 1, broadcast mode) whose sinks are all the
    listed peers, and a fixed pool of MAX_PEERS two-channel AooSinks
    (ID 1), one per slot, each bound to a peer through an atomic; the
    adapter between the host's blocks and
    AOO's fixed 64-sample blocks; the filter in front of AOO; the
    statistics. Client (liveclient.cpp) adds the socket and the threads.

  AOO knows the players by made-up IPv4 addresses: peer N is N.N.N.N in
  network order (sin_addr = htonl(N)), port 1; peer 0 is "everyone", the
  broadcast address. The send callback turns the address back into the
  peer ID that goes into our header.

  Internal: liveclient.h is the API.

*/

#ifndef JAMSA_LIVE_CORE_H
#define JAMSA_LIVE_CORE_H

#include "liveclient.h"

#include "aoo.h"
#include "aoo_sink.hpp"
#include "aoo_source.hpp"

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace jamsa_live {

namespace wire {

enum : uint8_t
{
  kVersion = 1,
  kHello = 1,     // player -> server; the answer is kHelloAck
  kHelloAck = 1,
  kData = 2,
};

const size_t kClientHeaderSize = 20;
const size_t kServerHeaderSize = 8;
const size_t kHelloSize = 40;    // a HELLO is exactly this long
const size_t kHelloAckSize = 24; // smaller than the HELLO: no amplification
const size_t kMaxDatagram = 1500;
const size_t kMaxPayload = 1452; // AOO packet in a DATA datagram

// HELLO: an opaque time stamp the server echoes, and the latest cookie the
// server sent us (0 for none: it proves our address); returns kHelloSize
size_t writeHello(uint8_t *out, uint32_t myId, uint64_t key, uint64_t stamp, uint64_t cookie);
// header of a DATA datagram; dest 0 = every other Live player in the room
void writeDataHeader(uint8_t *out, uint32_t myId, uint64_t key, uint32_t dest);

// a datagram from the server, checked: type, sizes
struct FromServer
{
  uint8_t type = 0;
  uint32_t from = 0;               // DATA: the sender's peer ID
  uint64_t stamp = 0;              // HELLO_ACK: our time stamp back
  uint64_t cookie = 0;             // HELLO_ACK: the cookie for our address
  const uint8_t *payload = nullptr; // DATA: the AOO packet
  size_t size = 0;
};
bool parseFromServer(const uint8_t *data, size_t size, FromServer &out);

// what a server (or the tests' fake one) reads
struct FromClient
{
  uint8_t type = 0;
  uint32_t sender = 0;
  uint64_t key = 0;
  uint32_t dest = 0;
  uint64_t stamp = 0;              // HELLO
  uint64_t cookie = 0;             // HELLO
  const uint8_t *payload = nullptr;
  size_t size = 0;
};
bool parseFromClient(const uint8_t *data, size_t size, FromClient &out);
// the 8 bytes a server puts in front of what it forwards; returns 8
size_t writeServerHeader(uint8_t *out, uint8_t type, uint32_t from);
// a whole HELLO_ACK; returns kHelloAckSize
size_t writeHelloAck(uint8_t *out, uint64_t stamp, uint64_t cookie);

} // namespace wire

// AOO's time stamps: 32.32 fixed point seconds
inline uint64_t ntpFromSeconds(double s)
{
  return (uint64_t)(s * 4294967296.0);
}

inline double secondsFromNtp(int64_t t)
{
  return (double)t / 4294967296.0;
}

class Core
{
public:
  static const int kMaxPeers = Client::MAX_PEERS;
  static const int kChunk = 64; // AOO's block: the adapter's B
  // the audio thread counts as idle this long after its last block's end
  // (longer than any host block: 8192 samples at 44.1 kHz are 0.19 s)
  static constexpr double kIdleSeconds = 0.25;

  // where the send thread's datagrams go: dest is a peer ID, 0 = everyone
  typedef void (*SendFn)(void *user, uint32_t dest, const uint8_t *data, size_t size);

  // what the filter in front of AOO decided
  enum Verdict
  {
    kAccepted,
    kBadSize,
    kNoSlot,      // from a player we have no slot for
    kFiltered,    // not an AOO message we take: IDs, kinds; the room is off
    kBadFormat,   // a /start with a format we don't play
  };

  Core();
  ~Core();
  Core(const Core &) = delete;
  Core &operator=(const Core &) = delete;

  // control thread
  void setSelf(uint32_t id);
  void configure(int kbps, int frameSamples);
  void setRoomOn(bool on);            // off: every slot goes quiet
  void setStreaming(bool on);         // our stream (room on, may send), while processBlock() gets 'send'
  bool streaming() const { return m_streaming; }
  void setPeers(const std::vector<Peer> &peers);
  void setJitterMs(int ms);
  int jitterMs() const { return m_jitterMs; }
  void poll(double now);              // seconds, any steady clock
  std::vector<PeerStats> stats(double now) const;
  void setDynamicResampling(bool on); // on by default (tests turn it off)

  // setup (not while processBlock() runs) and audio thread
  void prepare(double sampleRate, int maxBlock);
  // returns true when the send thread should run now
  bool processBlock(const float *inL, const float *inR, int frames, bool send, uint64_t ntpTime);
  // (any thread) whether the host calls processBlock(): its last block
  // ended less than kIdleSeconds before 'now'
  bool hostActive(uint64_t now) const;
  int slotOf(uint32_t peerId) const;
  bool slotHealthy(int slot) const;
  const float *stem(int slot, int channel) const;

  // send thread
  void send(SendFn fn, void *user);

  // receive thread: a DATA datagram's AOO packet from peer 'from', at
  // 'now' on the clock of processBlock()'s time stamps (0: no idle check)
  Verdict handleData(uint32_t from, const uint8_t *data, size_t size, uint64_t now = 0);

  // the filter alone (also for the tests): where a packet from 'from'
  // would go: -1 = our source, 0..kMaxPeers-1 = that slot's sink
  Verdict classify(uint32_t from, const uint8_t *data, size_t size, int *target) const;

  struct Totals
  {
    std::atomic<uint64_t> accepted{0}, badSize{0}, noSlot{0}, filtered{0}, badFormat{0};
    std::atomic<uint64_t> idle{0}; // for a sink while the audio thread was idle
    std::atomic<uint64_t> sent{0}, sendTooLarge{0};
    std::atomic<uint64_t> sendFailed{0}; // an exception out of AOO's send()
  };
  const Totals &totals() const { return m_totals; }

  // (tests) the AOO objects
  AooSource *source() { return m_source.get(); }
  AooSink *sink(int slot) { return m_slots[slot].sink.get(); }

private:
  struct SlotStats
  {
    double activeSince = -1;     // when the stream last went active
    double streamStart = -1;
    bool active = false;
    double rtt = 0;              // seconds
    double sourceLatency = 0, sinkLatency = 0, bufferLatency = 0;
    int formatBlock = 240, formatRate = 48000;
    int underruns = 0, overruns = 0, dropped = 0, resent = 0, xruns = 0;
    int streamDropped = 0, streamResent = 0; // since the stream started
    // recent dropouts (underruns, overruns, blocks lost for good)
    double dropouts[16] = {};
    int dropoutCount = 0;
    void dropout(double now, int n);
    int dropoutsSince(double t) const;
  };

  struct Slot
  {
    Core *core = nullptr;
    int index = 0;
    AooSink::Ptr sink;
    std::atomic<uint32_t> peer{0};  // 0 = free
    std::atomic<bool> healthy{false};
    // audio thread
    AooSample out[2][kChunk] = {};  // the sink's current block
    float *stems[2] = { nullptr, nullptr };
    bool bound = false;             // this block
    // control thread (m_statsMutex)
    SlotStats st;
  };

  static void AOO_CALL onSourceEvent(void *user, const AooEvent *e, AooThreadLevel level);
  static void AOO_CALL onSinkEvent(void *user, const AooEvent *e, AooThreadLevel level);
  static AooInt32 AOO_CALL aooSend(void *user, const AooByte *data, AooInt32 size,
                                    const void *address, AooAddrSize addrlen, AooFlag flags);
  void sinkEvent(Slot &slot, const AooEvent *e);
  void bind(int slot, uint32_t peer);
  void unbind(int slot);
  void setSourceSink(uint32_t peer, bool add);
  void streamOn(const std::atomic<bool> &other);
  bool listed(uint32_t peer) const;
  void updateHealth(double now);
  void applyBufferSize();

  AooSource::Ptr m_source;
  Slot m_slots[kMaxPeers];

  // control thread
  std::mutex m_ctlMutex;
  uint32_t m_self = 0;
  int m_kbps = 128, m_frame = 240;
  bool m_streaming = false;
  // Our stream runs while both are true: m_wantStream (setStreaming(), the
  // control thread) and m_sendOn (processBlock()'s 'send', the audio
  // thread). Each thread stores its own flag and then reads the other's;
  // whichever sees both true starts the stream (once more is harmless) and
  // checks again, and whichever turns its own off stops it (see
  // setStreaming()).
  std::atomic<bool> m_wantStream{false};
  std::atomic<bool> m_sendOn{false};
  bool m_roomOn = false;
  std::atomic<bool> m_rxOn{false};  // m_roomOn, for the receive thread
  int m_jitterMs = 20;
  std::vector<Peer> m_peers;
  // every peer in the list is a sink of our source, with a slot or not
  // (the server sends our stream to all of them); for the receive thread
  static const int kMaxListed = 64; // the server's LiveMaxPeers at most
  std::atomic<uint32_t> m_listed[kMaxListed];
  mutable std::mutex m_statsMutex;  // the slots' st, m_now
  double m_now = 0;

  // audio
  std::mutex m_audioMutex;          // prepare() vs processBlock() (try-lock)
  bool m_prepared = false;
  double m_sampleRate = 48000;      // (and m_maxBlock: also m_ctlMutex to write)
  int m_maxBlock = 0;
  std::vector<float> m_stemMemory;  // kMaxPeers * 2 * maxBlock
  std::vector<float> m_zeros;       // maxBlock
  AooSample m_in[2][kChunk] = {};
  int m_fill = 0;                   // samples in the current block
  bool m_blockValid = false;       // stem(): this block was rendered
  uint64_t m_lastTime = 0;          // the last block's time stamp
  int m_lastFrames = 0;
  std::atomic<double> m_rate{48000}; // m_sampleRate, for stats()
  std::atomic<uint64_t> m_audioDue{0}; // when the next block is due (0: none ran yet)

  Totals m_totals;
};

} // namespace jamsa_live

#endif
