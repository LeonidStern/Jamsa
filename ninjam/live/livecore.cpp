/*
    Jamsa Live - livecore.cpp
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

// winsock2.h before anything that could pull in windows.h
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#include "livecore.h"

#include "codec/aoo_opus.h"

// oscpack, which AOO brings
#include "osc/OscReceivedElements.h"

#include <string.h>

#include <algorithm>
#include <cmath>

static_assert(sizeof(AooSample) == sizeof(float), "AOO_SAMPLE_SIZE must be 32");

namespace jamsa_live {

// ---------------------------------------------------------------------------
// wire
// ---------------------------------------------------------------------------

namespace wire {

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }

static void putMagic(uint8_t *out, uint8_t type)
{
  out[0] = 'N';
  out[1] = 'L';
  out[2] = kVersion;
  out[3] = type;
}

static bool magicOk(const uint8_t *data)
{
  return data[0] == 'N' && data[1] == 'L' && data[2] == kVersion;
}

size_t writeHello(uint8_t *out, uint32_t myId, uint64_t key, uint64_t stamp, uint64_t cookie)
{
  memset(out, 0, kHelloSize);
  putMagic(out, kHello);
  put32(out + 4, myId);
  put64(out + 8, key);
  put32(out + 16, 0);
  put64(out + 20, stamp);
  put64(out + 28, cookie);
  // 36: four zero bytes
  return kHelloSize;
}

void writeDataHeader(uint8_t *out, uint32_t myId, uint64_t key, uint32_t dest)
{
  putMagic(out, kData);
  put32(out + 4, myId);
  put64(out + 8, key);
  put32(out + 16, dest);
}

bool parseFromServer(const uint8_t *data, size_t size, FromServer &out)
{
  if (!data || size < kServerHeaderSize || size > kMaxDatagram || !magicOk(data)) return false;
  out = FromServer();
  out.type = data[3];
  out.from = get32(data + 4);
  if (out.type == kHelloAck)
  {
    if (size != kHelloAckSize || out.from != 0) return false;
    out.stamp = get64(data + 8);
    out.cookie = get64(data + 16);
    return true;
  }
  if (out.type == kData)
  {
    if (size == kServerHeaderSize || size > kServerHeaderSize + kMaxPayload || out.from == 0) return false;
    out.payload = data + kServerHeaderSize;
    out.size = size - kServerHeaderSize;
    return true;
  }
  return false;
}

bool parseFromClient(const uint8_t *data, size_t size, FromClient &out)
{
  if (!data || size < kClientHeaderSize || size > kMaxDatagram || !magicOk(data)) return false;
  out = FromClient();
  out.type = data[3];
  out.sender = get32(data + 4);
  out.key = get64(data + 8);
  out.dest = get32(data + 16);
  if (out.type == kHello)
  {
    if (size != kHelloSize || out.dest != 0) return false;
    out.stamp = get64(data + 20);
    out.cookie = get64(data + 28);
    return true;
  }
  if (out.type == kData)
  {
    if (size == kClientHeaderSize || size > kClientHeaderSize + kMaxPayload) return false;
    out.payload = data + kClientHeaderSize;
    out.size = size - kClientHeaderSize;
    return true;
  }
  return false;
}

size_t writeServerHeader(uint8_t *out, uint8_t type, uint32_t from)
{
  putMagic(out, type);
  put32(out + 4, from);
  return kServerHeaderSize;
}

size_t writeHelloAck(uint8_t *out, uint64_t stamp, uint64_t cookie)
{
  writeServerHeader(out, kHelloAck, 0);
  put64(out + 8, stamp);
  put64(out + 16, cookie);
  return kHelloAckSize;
}

} // namespace wire

// ---------------------------------------------------------------------------
// Core
// ---------------------------------------------------------------------------

namespace {

// peer N <-> the address AOO knows it by (see livecore.h)
sockaddr_in addressOf(uint32_t peer)
{
  sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons(1);
  a.sin_addr.s_addr = htonl(peer);
  return a;
}

bool peerOf(const void *address, AooAddrSize addrlen, uint32_t *peer)
{
  if (!address || addrlen != (AooAddrSize)sizeof(sockaddr_in)) return false;
  sockaddr_in a;
  memcpy(&a, address, sizeof(a));
  if (a.sin_family != AF_INET || a.sin_port != htons(1)) return false;
  *peer = ntohl(a.sin_addr.s_addr);
  return true;
}

double ntpSeconds(AooNtpTime from, AooNtpTime to)
{
  return secondsFromNtp((int64_t)(to - from));
}

// for stem() before prepare()
const float g_zeros[8192] = {};

struct SendContext
{
  Core::SendFn fn;
  void *user;
  Core::Totals *totals;
};

} // namespace

void Core::SlotStats::dropout(double now, int n)
{
  for (int i = 0; i < n; i++)
    dropouts[dropoutCount++ % 16] = now;
}

int Core::SlotStats::dropoutsSince(double t) const
{
  int n = 0;
  const int count = std::min(dropoutCount, 16);
  for (int i = 0; i < count; i++)
    if (dropouts[i] >= t) n++;
  return n;
}

Core::Core()
{
  aoo_initialize(nullptr); // once per process (call_once in our copy)
  for (auto &id : m_listed) id.store(0);

  m_source = AooSource::create(1);
  m_source->setEventHandler(onSourceEvent, this, kAooEventModePoll);
  m_source->setDynamicResampling(kAooTrue);
  m_source->setStreamTimeSendInterval(0);
  m_source->setPacketSize((AooInt32)wire::kMaxPayload);
  // every data packet once, to peer 0: the server copies it to the room
  sockaddr_in everyone = addressOf(0);
  AooEndpoint ep;
  ep.address = &everyone;
  ep.addrlen = sizeof(everyone);
  ep.id = 1;
  m_source->setBroadcastEndpoint(&ep);

  for (int k = 0; k < kMaxPeers; k++)
  {
    Slot &s = m_slots[k];
    s.core = this;
    s.index = k;
    s.sink = AooSink::create(1);
    s.sink->setEventHandler(onSinkEvent, &s, kAooEventModePoll);
    s.sink->setDynamicResampling(kAooTrue);
    s.sink->setLatency(m_jitterMs * 0.001);
    s.sink->setPacketSize((AooInt32)wire::kMaxPayload);
    s.sink->setSourceTimeout(5.0);
  }

  std::lock_guard<std::mutex> lock(m_ctlMutex);
  AooFormatOpus f;
  AooFormatOpus_init(&f, 2, 48000, m_frame, OPUS_APPLICATION_RESTRICTED_LOWDELAY);
  f.bitrate = m_kbps * 1000;
  m_source->setFormat(f.header);
}

Core::~Core()
{
}

void Core::setSelf(uint32_t id)
{
  std::lock_guard<std::mutex> lock(m_ctlMutex);
  m_self = id;
}

void Core::configure(int kbps, int frameSamples)
{
  std::lock_guard<std::mutex> lock(m_ctlMutex);
  kbps = std::max(32, std::min(510, kbps));
  if (frameSamples != 120 && frameSamples != 240 && frameSamples != 480) frameSamples = 240;
  if (frameSamples != m_frame)
  {
    // a new format: the stream restarts
    m_frame = frameSamples;
    m_kbps = kbps;
    AooFormatOpus f;
    AooFormatOpus_init(&f, 2, 48000, m_frame, OPUS_APPLICATION_RESTRICTED_LOWDELAY);
    f.bitrate = m_kbps * 1000;
    m_source->setFormat(f.header);
  }
  else if (kbps != m_kbps)
  {
    // the encoder takes it from the next packet on, no restart
    m_kbps = kbps;
    opus_int32 bitrate = kbps * 1000;
    m_source->codecControl(kAooCodecOpus, OPUS_SET_BITRATE_REQUEST, 0, AOO_ARG(bitrate));
  }
}

void Core::setStreaming(bool on)
{
  std::lock_guard<std::mutex> lock(m_ctlMutex);
  if (on == m_streaming) return;
  m_streaming = on;
  m_wantStream.store(on);
  if (!on) m_source->stopStream(0);
  else if (m_sendOn.load()) streamOn(m_sendOn);
}

// Both flags (m_wantStream, m_sendOn) were seen true: start, then look at
// 'other' (the other thread's flag) again. If it went off in between, that
// thread may have stopped the stream before we started it: stop it again.
// (Otherwise it goes off later, and its stop comes after our start.) Only
// atomics change: also for the audio thread.
void Core::streamOn(const std::atomic<bool> &other)
{
  m_source->startStream(0, nullptr);
  if (!other.load()) m_source->stopStream(0);
}

void Core::setRoomOn(bool on)
{
  std::lock_guard<std::mutex> lock(m_ctlMutex);
  if (on == m_roomOn) return;
  m_roomOn = on;
  m_rxOn.store(on);
  if (!on)
  {
    // the others stop too, but the server may already drop their /stop:
    // let every slot go quiet now rather than run dry
    for (Slot &s : m_slots)
    {
      s.healthy.store(false);
      s.sink->removeSource(nullptr);
    }
    std::lock_guard<std::mutex> stats(m_statsMutex);
    for (Slot &s : m_slots)
    {
      s.st.active = false;
      s.st.activeSince = -1;
    }
  }
}

void Core::setDynamicResampling(bool on)
{
  m_source->setDynamicResampling(on);
  for (Slot &s : m_slots) s.sink->setDynamicResampling(on);
}

void Core::setJitterMs(int ms)
{
  std::lock_guard<std::mutex> lock(m_ctlMutex);
  ms = std::max(5, std::min(200, ms));
  if (ms == m_jitterMs) return;
  m_jitterMs = ms;
  // NB: resets the streams' buffers (one short gap)
  for (Slot &s : m_slots) s.sink->setLatency(ms * 0.001);
  applyBufferSize();
}

// AOO sizes a sink's jitter buffer for its own blocks (twice the latency
// unless told otherwise), but a whole host block of packets goes into it at
// once, at the block's first 64 samples: with 2048-sample blocks, or with
// a jitter setting of 5 ms, it overflowed again and again and the slot never
// got healthy. Room for that too, never less than AOO's own size.
// With m_ctlMutex held.
void Core::applyBufferSize()
{
  const double latency = m_jitterMs * 0.001;
  const double host = m_sampleRate > 0 ? m_maxBlock / m_sampleRate : 0;
  const double size = std::max(2 * latency, latency + 2 * host + 0.005);
  for (Slot &s : m_slots) s.sink->setBufferSize(size);
}

void Core::bind(int k, uint32_t peer)
{
  Slot &s = m_slots[k];
  {
    std::lock_guard<std::mutex> stats(m_statsMutex);
    s.st = SlotStats();
  }
  s.healthy.store(false);
  s.peer.store(peer, std::memory_order_release);
}

void Core::unbind(int k)
{
  Slot &s = m_slots[k];
  const uint32_t peer = s.peer.exchange(0, std::memory_order_acq_rel);
  s.healthy.store(false);
  if (!peer) return;
  // the old peer's source goes with the next block, so the slot starts
  // clean for the next peer (see AooSink::removeSource())
  s.sink->removeSource(nullptr);
  std::lock_guard<std::mutex> stats(m_statsMutex);
  s.st = SlotStats();
}

void Core::setSourceSink(uint32_t peer, bool add)
{
  sockaddr_in a = addressOf(peer);
  AooEndpoint ep;
  ep.address = &a;
  ep.addrlen = sizeof(a);
  ep.id = 1;
  if (add) m_source->addSink(ep, kAooTrue); // active: AOO ignores an inactive sink's requests
  else m_source->removeSink(ep);
}

bool Core::listed(uint32_t peer) const
{
  if (!peer) return false;
  for (const auto &id : m_listed)
    if (id.load(std::memory_order_acquire) == peer) return true;
  return false;
}

void Core::setPeers(const std::vector<Peer> &peers)
{
  std::lock_guard<std::mutex> lock(m_ctlMutex);
  auto wanted = [&](uint32_t id) {
    if (!id || id == m_self) return false;
    for (const Peer &p : peers) if (p.id == id) return true;
    return false;
  };
  // free the slots of players who left
  for (int k = 0; k < kMaxPeers; k++)
  {
    const uint32_t id = m_slots[k].peer.load();
    if (id && !wanted(id)) unbind(k);
  }
  // new players take free slots, in the order given
  for (const Peer &p : peers)
  {
    if (!wanted(p.id) || slotOf(p.id) >= 0) continue;
    for (int k = 0; k < kMaxPeers; k++)
    {
      if (m_slots[k].peer.load() == 0)
      {
        bind(k, p.id);
        break;
      }
    }
  }
  // Every listed player is a sink of our source, with a slot for it or
  // not: the server sends our stream to all of them, and one without our
  // /start would ask for it with every packet of ours and never hear us.
  std::vector<uint32_t> ids;
  for (const Peer &p : peers)
    if (wanted(p.id) && std::find(ids.begin(), ids.end(), p.id) == ids.end() && ids.size() < (size_t)kMaxListed)
      ids.push_back(p.id);
  for (auto &l : m_listed)
  {
    const uint32_t id = l.load();
    if (id && std::find(ids.begin(), ids.end(), id) == ids.end())
    {
      l.store(0, std::memory_order_release);
      setSourceSink(id, false);
    }
  }
  for (uint32_t id : ids)
  {
    if (listed(id)) continue;
    for (auto &l : m_listed)
    {
      if (l.load() == 0)
      {
        l.store(id, std::memory_order_release);
        setSourceSink(id, true);
        break;
      }
    }
  }
  std::lock_guard<std::mutex> stats(m_statsMutex);
  m_peers = peers;
}

int Core::slotOf(uint32_t peerId) const
{
  if (!peerId) return -1;
  for (int k = 0; k < kMaxPeers; k++)
    if (m_slots[k].peer.load(std::memory_order_acquire) == peerId) return k;
  return -1;
}

bool Core::slotHealthy(int slot) const
{
  if (slot < 0 || slot >= kMaxPeers) return false;
  return m_slots[slot].healthy.load(std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// audio

void Core::prepare(double sampleRate, int maxBlock)
{
  if (sampleRate <= 0 || maxBlock <= 0) return;
  // (in this order; processBlock() only try-locks the audio mutex)
  std::lock_guard<std::mutex> control(m_ctlMutex);
  m_blockValid.store(false);
  std::lock_guard<std::mutex> lock(m_audioMutex);
  m_blockValid.store(false);
  m_zerosSize.store(0); // stem(): g_zeros while m_zeros changes
  m_sampleRate = sampleRate;
  m_rate.store(sampleRate);
  m_maxBlock = maxBlock;
  m_source->setup(2, sampleRate, kChunk, kAooFixedBlockSize);
  // The source's queue to the network thread takes a whole host block at once, and what the
  // network thread hasn't sent of the last one: AOO's 25 ms is less than a long host block
  // (2048 samples are 43 ms at 48 kHz), and what doesn't fit is lost.
  m_source->setBufferSize(std::max(0.025, 2.0 * maxBlock / sampleRate + 0.01));
  for (Slot &s : m_slots) s.sink->setup(2, sampleRate, kChunk, kAooFixedBlockSize);
  m_stemMemory.assign((size_t)kMaxPeers * 2 * maxBlock, 0.0f);
  m_zeros.assign(maxBlock, 0.0f);
  for (int k = 0; k < kMaxPeers; k++)
  {
    for (int c = 0; c < 2; c++)
    {
      m_slots[k].stems[c] = m_stemMemory.data() + ((size_t)k * 2 + c) * maxBlock;
      memset(m_slots[k].out[c], 0, sizeof(m_slots[k].out[c]));
    }
    m_slots[k].bound = false;
  }
  memset(m_in, 0, sizeof(m_in));
  m_fill = 0;
  m_lastFrames = 0;
  m_prepared = true;
  m_zerosSize.store((int)m_zeros.size());
  applyBufferSize();
}

bool Core::processBlock(const float *inL, const float *inR, int frames, bool send, uint64_t t)
{
  std::unique_lock<std::mutex> lock(m_audioMutex, std::try_to_lock);
  if (!lock.owns_lock() || !m_prepared || frames <= 0 || frames > m_maxBlock)
  {
    m_blockValid.store(false);
    return false;
  }
  m_blockValid.store(true);

  // The host stopped calling us for a while (processing suspended, an
  // xrun): tell AOO, which fills the gap with empty blocks on the sending
  // side and skips it on the receiving side, and restarts its clocks. Not
  // telling it of a short one (a DAW that missed 32 ms) is worse: the time
  // filter takes the jump for a slow clock, and the sinks then play their
  // buffers down over half a minute, into dropouts. Up to two blocks' worth
  // is how hosts call (in bursts, within one device buffer), not a gap.
  if (m_lastFrames > 0)
  {
    const double gap = secondsFromNtp((int64_t)(t - m_lastTime)) - m_lastFrames / m_sampleRate;
    if (gap > std::max(0.02, 2.0 * m_maxBlock / m_sampleRate))
    {
      const AooInt32 n = (AooInt32)(std::min(gap, 0.5) * m_sampleRate);
      m_source->reportXRun(n);
      for (Slot &s : m_slots) s.sink->reportXRun(n);
    }
  }
  m_lastTime = t;
  m_lastFrames = frames;
  m_audioDue.store(t + ntpFromSeconds(frames / m_sampleRate), std::memory_order_relaxed);

  // nothing to send (the host stopped, transmit off): no stream either,
  // rather than one of silence
  const bool sending = send && inL;
  if (sending != m_sendOn.load(std::memory_order_relaxed))
  {
    m_sendOn.store(sending);
    if (m_wantStream.load())
    {
      if (sending) streamOn(m_wantStream);
      else m_source->stopStream(0);
    }
  }

  for (Slot &s : m_slots) s.bound = s.peer.load(std::memory_order_acquire) != 0;
  const float *inRight = inR ? inR : inL;
  bool wake = false;
  int done = 0;
  while (done < frames)
  {
    const int n = std::min(frames - done, kChunk - m_fill);
    if (sending)
    {
      memcpy(m_in[0] + m_fill, inL + done, n * sizeof(float));
      memcpy(m_in[1] + m_fill, inRight + done, n * sizeof(float));
    }
    else
    {
      memset(m_in[0] + m_fill, 0, n * sizeof(float));
      memset(m_in[1] + m_fill, 0, n * sizeof(float));
    }
    for (Slot &s : m_slots)
    {
      if (!s.bound) continue;
      memcpy(s.stems[0] + done, s.out[0] + m_fill, n * sizeof(float));
      memcpy(s.stems[1] + done, s.out[1] + m_fill, n * sizeof(float));
    }
    m_fill += n;
    done += n;
    if (m_fill == kChunk)
    {
      // a block of AOO's: time stamps advance with the samples
      const uint64_t tk = t + ntpFromSeconds(done / m_sampleRate);
      AooSample *in[2] = { m_in[0], m_in[1] };
      if (m_source->process(in, kChunk, tk) != kAooErrorIdle) wake = true;
      for (Slot &s : m_slots)
      {
        AooSample *out[2] = { s.out[0], s.out[1] };
        if (s.sink->process(out, kChunk, tk, nullptr, nullptr) != kAooErrorIdle) wake = true;
      }
      m_fill = 0;
    }
  }
  return wake;
}

const float *Core::stem(int slot, int channel) const
{
  if (!m_blockValid.load() || slot < 0 || slot >= kMaxPeers || channel < 0 || channel > 1
      || !m_slots[slot].bound)
  {
    // (m_zeros only for blocks longer than g_zeros, and not while prepare()
    // changes it)
    const int n = m_zerosSize.load();
    return n > (int)(sizeof(g_zeros) / sizeof(g_zeros[0])) ? m_zeros.data() : g_zeros;
  }
  return m_slots[slot].stems[channel];
}

// ---------------------------------------------------------------------------
// network

AooInt32 AOO_CALL Core::aooSend(void *user, const AooByte *data, AooInt32 size,
                                 const void *address, AooAddrSize addrlen, AooFlag)
{
  SendContext *ctx = (SendContext *)user;
  uint32_t dest = 0;
  if (size <= 0 || !peerOf(address, addrlen, &dest)) return 0;
  if ((size_t)size > wire::kMaxPayload)
  {
    ctx->totals->sendTooLarge++;
    return 0;
  }
  ctx->totals->sent++;
  ctx->fn(ctx->user, dest, (const uint8_t *)data, (size_t)size);
  return size;
}

void Core::send(SendFn fn, void *user)
{
  SendContext ctx = { fn, user, &m_totals };
  // AOO's send() can throw (std::bad_alloc while it grows a buffer): out of
  // the send thread that would end the host. What wasn't sent is lost, as
  // on the network; the next pass goes on.
  try
  {
    m_source->send(aooSend, &ctx);
  }
  catch (...)
  {
    m_totals.sendFailed++;
  }
  for (Slot &s : m_slots)
  {
    try
    {
      s.sink->send(aooSend, &ctx);
    }
    catch (...)
    {
      m_totals.sendFailed++;
    }
  }
}

// a /start's format: what we play (Opus, mono or stereo, 48 kHz, up to 10 ms)
static bool startFormatOk(const osc::ReceivedMessage &msg)
{
  // <src> <version> <stream_id> <seq_start> <format_id> <nchannels>
  // <samplerate> <blocksize> <codec> ...
  osc::int32 v[8] = {};
  const char *codec = nullptr;
  int i = 0;
  for (auto it = msg.ArgumentsBegin(); it != msg.ArgumentsEnd() && i <= 8; ++it, ++i)
  {
    if (i == 1 || i == 8)
    {
      if (!it->IsString()) return false;
      if (i == 8) codec = it->AsStringUnchecked();
    }
    else
    {
      if (!it->IsInt32()) return false;
      v[i] = it->AsInt32Unchecked();
    }
  }
  if (i <= 8 || !codec) return false;
  return !strcmp(codec, kAooCodecOpus) && v[5] >= 1 && v[5] <= 2 && v[6] == 48000
    && v[7] >= 120 && v[7] <= 480;
}

Core::Verdict Core::classify(uint32_t from, const uint8_t *data, size_t size, int *target) const
{
  if (!data || size == 0 || size > wire::kMaxPayload) return kBadSize;
  // for a sink: from its slot's player; for our source: from any listed
  // player (each is a sink of it)
  const int slot = slotOf(from);
  if (slot < 0 && !listed(from)) return kNoSlot;
  // room off: no streams either way (what is still on its way is dropped)
  if (!m_rxOn.load(std::memory_order_relaxed)) return kFiltered;

  if (data[0] & kAooBinMsgDomainBit)
  {
    // binary: only /data, between ID 1 and ID 1, so always the short
    // header (type, command, to, from: one byte each)
    if (size < 4) return kBadSize;
    const int type = data[0] & ~kAooBinMsgDomainBit;
    if ((data[1] & kAooBinMsgSizeBit) || data[1] != kAooBinMsgCmdData || data[2] != 1 || data[3] != 1)
      return kFiltered;
    if (type == kAooMsgTypeSink)
    {
      if (slot < 0) return kNoSlot;
      *target = slot;
      return kAccepted;
    }
    if (type == kAooMsgTypeSource) { *target = -1; return kAccepted; }
    return kFiltered;
  }

  // OSC: /aoo/sink/1/... for the slot's sink, /aoo/src/1/... for our source
  AooMsgType type = 0;
  AooId id = -1;
  AooInt32 onset = 0;
  if (aoo_parsePattern(data, (AooInt32)size, &type, &id, &onset) != kAooOk || id != 1)
    return kFiltered;
  try
  {
    osc::ReceivedPacket packet((const char *)data, size);
    if (!packet.IsMessage()) return kFiltered;
    osc::ReceivedMessage msg(packet);
    const char *pattern = msg.AddressPattern();
    if ((size_t)onset > strlen(pattern)) return kFiltered;
    const char *cmd = pattern + onset;
    // the first argument is always the other side's ID
    auto first = msg.ArgumentsBegin();
    if (first == msg.ArgumentsEnd() || !first->IsInt32() || first->AsInt32Unchecked() != 1)
      return kFiltered;
    if (type == kAooMsgTypeSink)
    {
      if (slot < 0) return kNoSlot;
      if (!strcmp(cmd, kAooMsgStart))
      {
        if (!startFormatOk(msg)) return kBadFormat;
      }
      else if (strcmp(cmd, kAooMsgStop) && strcmp(cmd, kAooMsgData) && strcmp(cmd, kAooMsgPing)
               && strcmp(cmd, kAooMsgPong))
        return kFiltered;
      *target = slot;
      return kAccepted;
    }
    if (type == kAooMsgTypeSource)
    {
      // no invitations: we send to the room's players, not on request
      if (strcmp(cmd, kAooMsgStart) && strcmp(cmd, kAooMsgStop) && strcmp(cmd, kAooMsgData)
          && strcmp(cmd, kAooMsgPing) && strcmp(cmd, kAooMsgPong))
        return kFiltered;
      *target = -1;
      return kAccepted;
    }
  }
  catch (...)
  {
  }
  return kFiltered;
}

bool Core::hostActive(uint64_t now) const
{
  const uint64_t due = m_audioDue.load(std::memory_order_relaxed);
  return due && (int64_t)(now - due) <= (int64_t)ntpFromSeconds(kIdleSeconds);
}

Core::Verdict Core::handleData(uint32_t from, const uint8_t *data, size_t size, uint64_t now)
{
  int target = 0;
  const Verdict v = classify(from, data, size, &target);
  // The host isn't calling processBlock() (processing stopped, or not
  // started yet): AOO would keep what goes to the sinks until the audio
  // thread runs again and then work through all of it in one block. The
  // streams pick up again from what comes next. What goes to our source
  // still goes through: the send thread answers it.
  if (v == kAccepted && target >= 0 && now && !hostActive(now))
  {
    m_totals.idle++;
    return kFiltered;
  }
  switch (v)
  {
    case kAccepted: m_totals.accepted++; break;
    case kBadSize: m_totals.badSize++; return v;
    case kNoSlot: m_totals.noSlot++; return v;
    case kBadFormat: m_totals.badFormat++; return v;
    default: m_totals.filtered++; return v;
  }
  const sockaddr_in a = addressOf(from);
  try
  {
    if (target < 0) m_source->handleMessage(data, (AooInt32)size, &a, sizeof(a));
    else m_slots[target].sink->handleMessage(data, (AooInt32)size, &a, sizeof(a));
  }
  catch (...)
  {
    // AOO catches everything itself; this is the last line
  }
  return kAccepted;
}

// ---------------------------------------------------------------------------
// events, statistics

void AOO_CALL Core::onSourceEvent(void *, const AooEvent *, AooThreadLevel)
{
  // Nothing to keep: what a listener's sink says about our stream (its
  // pings) is in that listener's own statistics.
}

void AOO_CALL Core::onSinkEvent(void *user, const AooEvent *e, AooThreadLevel)
{
  Slot &s = *(Slot *)user;
  s.core->sinkEvent(s, e);
}

void Core::sinkEvent(Slot &slot, const AooEvent *e)
{
  // every sink event but an error names the source: only the slot's
  // current peer counts
  if (e->type == kAooEventError) return;
  uint32_t peer = 0;
  if (!peerOf(e->endpoint.endpoint.address, e->endpoint.endpoint.addrlen, &peer)
      || peer != slot.peer.load())
    return;
  SlotStats &st = slot.st;
  const double now = m_now;
  switch (e->type)
  {
    case kAooEventStreamStart:
      st.streamStart = now;
      st.streamDropped = st.streamResent = 0;
      break;
    case kAooEventStreamState:
      if (e->streamState.state == kAooStreamStateActive)
      {
        if (!st.active)
        {
          // the loss rate counts from here
          st.activeSince = now;
          st.streamStart = now;
          st.streamDropped = st.streamResent = 0;
        }
        st.active = true;
      }
      else
      {
        st.active = false;
        st.activeSince = -1;
      }
      break;
    case kAooEventStreamStop:
    case kAooEventSourceRemove:
      st.active = false;
      st.activeSince = -1;
      break;
    case kAooEventFormatChange:
      if (e->formatChange.format && e->formatChange.format->blockSize > 0
          && e->formatChange.format->sampleRate > 0)
      {
        st.formatBlock = e->formatChange.format->blockSize;
        st.formatRate = e->formatChange.format->sampleRate;
      }
      break;
    case kAooEventStreamLatency:
      st.sourceLatency = e->streamLatency.sourceLatency;
      st.sinkLatency = e->streamLatency.sinkLatency;
      st.bufferLatency = e->streamLatency.bufferLatency;
      break;
    case kAooEventBufferUnderrun:
      st.underruns++;
      st.dropout(now, 1);
      break;
    case kAooEventBufferOverrun:
      st.overruns++;
      st.dropout(now, 1);
      break;
    case kAooEventBlockDrop:
      st.dropped += e->blockDrop.count;
      st.streamDropped += e->blockDrop.count;
      st.dropout(now, std::min(e->blockDrop.count, 16));
      break;
    case kAooEventBlockResend:
      st.resent += e->blockResend.count;
      st.streamResent += e->blockResend.count;
      break;
    case kAooEventBlockXRun:
      // empty blocks for the sender's xrun (its host stopped): a gap we
      // hear, also when the buffer was long enough not to run dry. One
      // dropout a gap: its blocks come over several polls.
      st.xruns += e->blockXRun.count;
      if (now - st.lastXRun > 0.1) st.dropout(now, 1);
      st.lastXRun = now;
      break;
    case kAooEventSourcePing:
    {
      // our ping to its source and back, less the time it took to answer
      const double rtt = ntpSeconds(e->sourcePing.t1, e->sourcePing.t4)
        - ntpSeconds(e->sourcePing.t2, e->sourcePing.t3);
      if (rtt >= 0 && rtt < 10) st.rtt = st.rtt > 0 ? 0.7 * st.rtt + 0.3 * rtt : rtt;
      break;
    }
    default:
      break;
  }
}

void Core::updateHealth(double now)
{
  for (Slot &s : m_slots)
  {
    const SlotStats &st = s.st;
    const bool healthy = m_roomOn && s.peer.load() != 0 && st.active && st.activeSince >= 0
      && now - st.activeSince >= 1.0 && st.dropoutsSince(now - 2.0) <= 2;
    s.healthy.store(healthy, std::memory_order_relaxed);
  }
}

void Core::poll(double now)
{
  std::lock_guard<std::mutex> lock(m_ctlMutex);
  std::lock_guard<std::mutex> stats(m_statsMutex);
  m_now = now;
  m_source->pollEvents();
  for (Slot &s : m_slots) s.sink->pollEvents();
  updateHealth(now);
}

std::vector<PeerStats> Core::stats(double now) const
{
  std::lock_guard<std::mutex> lock(m_statsMutex);
  std::vector<PeerStats> out;
  for (const Peer &p : m_peers)
  {
    PeerStats ps;
    ps.id = p.id;
    ps.name = p.name;
    ps.slot = slotOf(p.id);
    if (ps.slot >= 0)
    {
      const Slot &s = m_slots[ps.slot];
      const SlotStats &st = s.st;
      ps.streaming = st.active;
      ps.healthy = s.healthy.load();
      ps.rttMs = st.rtt * 1000;
      ps.bufferMs = st.bufferLatency * 1000;
      ps.codecMs = (st.sourceLatency + st.sinkLatency) * 1000;
      // half the round trip, both ends' processing, and both adapters
      // (B samples each, half of that on average)
      ps.oneWayMs = ps.rttMs / 2 + ps.codecMs + ps.bufferMs + 1000.0 * kChunk / m_rate.load();
      if (st.streamStart >= 0 && now > st.streamStart)
      {
        const double blocks = (now - st.streamStart) * st.formatRate / st.formatBlock;
        if (blocks >= 1)
          ps.lossPct = std::min(100.0, 100.0 * (st.streamDropped + st.streamResent) / blocks);
      }
      ps.underruns = st.underruns;
      ps.overruns = st.overruns;
      ps.dropped = st.dropped;
      ps.resent = st.resent;
    }
    out.push_back(ps);
  }
  return out;
}

} // namespace jamsa_live
