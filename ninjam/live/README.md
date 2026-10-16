# ninjam/live: Jamsa's Live mode

Live mode lets the players in a NINJAM room hear each other in real time,
next to NINJAM's interval sync. Each Jamsa sends its audio in small Opus
packets over UDP to the NINJAM server it is connected to, and the server
forwards them to the other players in the room. On the receiving side a jitter
buffer smooths out the network, resends fill most gaps and Opus' packet loss
concealment the rest.

The streaming itself is AOO ("Audio over OSC"), trimmed to what Jamsa needs:
an `AooSource` that sends, an `AooSink` that receives and plays back, and the
Opus codec. Jamsa never lets AOO open a socket: it hands AOO's packets to its
own UDP socket and the packets that arrive back to AOO.

## Where AOO comes from

- https://github.com/Spacechild1/aoo, branch `develop`, commit `6101586`
  (version 2.0.0-test4), by Christof Ressi, Winfried Ritsch and others.
- Licence: BSD (`aoo/LICENSE`); oscpack, which AOO uses to read and write OSC,
  is MIT (`aoo/deps/oscpack/LICENSE`). The upstream file headers are kept.
- The import commit ("Live: import AOO 6101586's audio core, unmodified")
  holds the upstream files byte for byte, so `git diff` against it shows every
  change we made.

Layout: `aoo/include` (public headers), `aoo/src` (upstream's `aoo/src`),
`aoo/common`, `aoo/deps/oscpack`. Upstream's tree shape is kept otherwise.

## What was left out

- AOO's networking layer (`AOO_NET`: AooClient, AooServer, peers, groups,
  relay, `aoo/src/net`, the `server` folder). `AOO_NET` defaults to 0 and
  is an error if turned on.
- The C wrappers of the source and the sink, the codec registry with the
  PCM and null codecs (Opus is wired in directly), the sockaddr, NTP and
  strerror C helpers, and the socket, semaphore and NTP server code in
  `common/`.
- AOO's own copy of Opus (we link the Opus 1.5.2 the plug-in already builds),
  md5, portaudio, the Pd and SuperCollider externals, examples, docs, tests and
  AOO's own CMake files.

## Building

`CMakeLists.txt` builds the static library `ninjam_live` (AOO and oscpack),
with `AOO_NET=0`, `AOO_USE_OPUS=1`, `AOO_SAMPLE_SIZE=32` and AOO's log level
at Error. It probes whether 64-bit and double atomics are lock free
(`AOO_HAVE_ATOMIC_INT64`, `AOO_HAVE_ATOMIC_DOUBLE`) and links libatomic only
if the compiler needs it. The plug-in's build (`ninjam/vst3`) adds this folder
and its tests; it can also be built alone:

    cmake -S ninjam/live -B build-live -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build-live && ctest --test-dir build-live

Tests (`tests/`):

- `live_aoo_test`: AOO's lock-free queue and RT memory pool under threads
  (ported from AOO's own tests, with real checks instead of printouts), and
  a test for each AOO bug we fixed, mostly through a source and sinks on a
  virtual clock and network (`live_harness.h`).
- `live_fuzz_test [iterations] [seed] [test]`: a hand-made packet for each
  hardening change below, then hostile datagrams into a running source and
  three sinks: captured ones mutated, ones built field by field around every
  limit, and random bytes, from the peers' addresses and from strangers'.
  It checks that the outputs stay finite, that nothing logs at Error level,
  and that a new stream plays normally afterwards. It matters most in a
  build with `-fsanitize=address,undefined`.
- `live_rt_test`: counts the heap calls (the program replaces `operator
  new` and `delete`) made inside the source's and the sinks' `process()`:
  none once a stream runs, on a clean network, with loss, jitter, reordering
  and duplicates, while the stream restarts and the source has xruns, and
  with stream messages. Also the other real-time changes below. `LIVE_RT_TRACE=1` prints where a
  counted call came from (glibc).
- `live_unit_test [test]`: what Live adds to AOO: coupled stereo Opus and
  its bitrate (encoded sizes, the decoder's channel layout, through the
  source too), broadcast mode with three sinks (one upload per block, a lost
  copy resent to its sink only, a sink that joins late, a channel offset,
  turning it on and off).

## Changes from upstream

Everything we changed in AOO, and why. Ideas we had but did not apply are
noted as such.

### The trim

- `aoo_config.h`: `AOO_NET` defaults to 0 and errors out if set (the net
  sources are gone). The `#error` text for `AOO_PACKET_SIZE` said the
  opposite of the check.
- `aoo.cpp`: `aoo_initialize` runs once under `std::call_once`; upstream used
  a plain `static bool`, a data race if two plug-in instances start at the
  same time.
- `aoo.cpp`: `aoo_parsePattern` returns `kAooErrorBadArgument` for anything
  but `/aoo/src` and `/aoo/sink`. Upstream, with `AOO_NET` off, it fell
  through and returned `kAooOk` without setting the type, offset or id.
- `log.hpp`: `LOG_ALL` named `kAooLogLevelNone`, which does not exist.
- `codec/opus.cpp`: `aoo_opusInterface()` instead of the codec registry.

### Bugs fixed

- `rt_memory_pool.hpp`, `free_list::empty()`: returned the opposite, so freed
  blocks of a larger size class were never split for smaller requests and the
  pool spilled into the heap under churn.
- `rt_memory_pool.hpp`, `tagged_bitset`: had two padding bytes. libstdc++ 13
  and later clear the padding of the expected and desired values in
  `compare_exchange`, not that of the stored value, so `update_bitset()` spun
  forever for any pool outside zero-initialized memory (AOO's own pool is a
  global, which hid it; our pool test hung). The tag is now as wide as the
  bits (still 16 bits used), a `static_assert` keeps padding out, and the
  struct is aligned to its size so that clang inlines the compare-exchange
  instead of calling libatomic.
- `packet_buffer.cpp`, `received_block::init`: compared the packet's
  sequence number with itself, so a reused slot kept the old block's resend
  timer and try count.
- `packet_buffer.cpp`, `jitter_buffer::find`: searched up to the vector's
  capacity instead of its size after the buffer shrank (ASan
  container-overflow under reordering and loss).
- `sink.cpp`, OSC `/data`: a block without frame fields was taken for one
  frame even when empty, so the empty blocks a source sends after an xrun
  never completed in the OSC format; they were counted as dropped and asked
  for again. Also set the wrong flag (stream message instead of frames) when
  the frame fields were there.
- `sink.cpp`, `source_desc::codec_control`: ran the sink's codec controls
  through the encoder's control function on the decoder's state (wrong
  answers; setters wrote into the Opus decoder).
- `source.cpp`, `send_start`: `startStream()` hides the sample offset in the
  metadata's data pointer. The first `/start` took it out and restored the
  pointer, so every later `/start` of the same stream (a sink that joins, or
  one that asks again after losing the first) sent the pointer as the
  offset, and that sink never went active. The offset is now taken out once,
  in `make_new_stream()`.
- `aoo_source.hpp`, `getSinkChannelOffset()` sent the Set control (it set
  the offset to the caller's variable); `aoo_sink.hpp`,
  `getDynamicResampling()` took its result by value.
- `resampler.cpp`, `dynamic_resampler::setup`: kept the buffer when only the
  channel count changed, though the buffer's front padding and its size on
  free depend on it (writes before the block, then a bad free).
- `memory.hpp`: the allocators' converting `operator=` did not return.
- `time_dll.hpp`: the denormal flush tested `e2_ <= min` instead of its
  magnitude, so it zeroed any negative value, not just tiny ones.
- `codec/opus.cpp`: seven codec interface functions lacked `AOO_CALL`, which
  only shows with a non-default calling convention (32-bit Windows).
- `common/lockfree.hpp`, `concurrent_queue::reserve()`: with more free
  nodes than asked for, the count wrapped around and it allocated until
  memory ran out.
- `rt_memory_pool.hpp`: the leak check (debug builds) counted requests that
  failed (the pool was full, or size 0) as blocks in use and asserted at
  exit.
- `events.hpp`, `ievent::cast()`: went through a `static_cast` to a type the
  event is not (undefined behavior, reported by UBSan for every event). Each
  event type now returns its own `AooEvent`.

`live_aoo_test` has a test for each of these that fails without the fix,
except the flag, the denormal flush and `AOO_CALL`, which have no visible
effect on our builds.

### Hardening

Everything AOO receives comes from the other players, through our server,
so a broken or hostile client can send it anything. Our copy checks what a
datagram says before anything sizes memory, indexes an array or reaches the
audio thread with it. `live_fuzz_test` has a packet for each of these; the
ones marked "(fuzzer)" were found by it.

- `Sink`/`Source::handleMessage`: drop empty datagrams and ones larger than
  `AOO_MAX_PACKET_SIZE`, and catch every exception (only oscpack's were
  caught, anything else went up into the caller's receive loop).
- `aoo_parsePattern`: read the ID with `sscanf`, past the end of a datagram
  (they are not NUL-terminated), and `%d` overflow is undefined; it is now
  parsed within the size. `check_version`: `strtol` instead of `sscanf`.
- oscpack, `ReceivedMessage::Init` (fuzzer): the blob size checks built the
  exception without throwing it, so a blob could claim more bytes than the
  message has and the sink copied them (an out-of-bounds read). A message
  with an empty type tag string and bytes after it took the message's last
  byte for its type tags; an `i` there read the first argument through a
  null pointer.
- Sink `/data` (`check_data_packet()`): stream ID and sequence not negative;
  channel onset 0..255 (a negative one added the stream into memory before
  the output buffers, on the audio thread); block size up to
  `AOO_MAX_BLOCK_DATA_SIZE` (64 KB; the block is put together with
  `alloca`); 1 to 256 frames, the frame index below the count (it indexed
  the frame array) and frame sizes within the block; stream message size a
  multiple of 4 within the block; a sample rate that is finite and
  positive. Binary `/data` read the time stamp before checking that the
  packet holds it.
- Sink jitter buffer: a frame whose frame count or sizes disagree with the
  frames already received for its block is dropped, and the frames must add
  up to the block size before they are copied (`copy_frames()` overflowed
  the stack buffer on the audio thread otherwise).
- `data_frame_storage::add_frame` (fuzzer): took `&nullptr->header`
  (undefined; blocks of more than 6 frames).
- Sample rates: one far from the nominal rate (more than 10 %) is replaced
  by the nominal rate, and `dynamic_resampler::update()` ignores a step
  outside half to twice the nominal one, or NaN (the resampler reads its
  buffer by that step).
- Sink `/start`: formats outside Opus' limits are rejected (1 to
  `AOO_MAX_STREAM_CHANNELS` = 8 channels, Opus' sample rates, 2.5 to 60 ms
  blocks) instead of being "fixed" by the codec; the channel count and block
  size size `alloca` buffers on the audio thread. A rejected format also
  forgets its format ID: a good format, a bad one, then the good one's ID
  again ran with no decoder (a null pointer dereference). Latency, codec
  delay and sample offset are clamped to 0..10 seconds (negative ones, and
  sums that overflowed once resampled (fuzzer)).
- Decoding: a packet of another frame size decodes into fewer samples; the
  rest of the buffer was stack garbage (an assert in debug builds only). It
  is now zero filled, like a block that fails to decode.
- Stream messages: their headers were read without a bound; a message of
  type `0xFFFF` (the sink's own stream time type) was later read as a time
  stamp it didn't carry; a sink without a message handler called a null
  pointer; and a source could keep any amount scheduled (now at most
  `AOO_MAX_PENDING_STREAM_MESSAGES_SIZE`, 16 KB, per source).
- Source: the count of a binary resend request was multiplied by 8 before
  the size check (overflow with a 32-bit `size_t`); `/invite` with the
  token -1 (fuzzer) added a sink without telling the user (an assert in
  debug builds).
- Opus `deserialize`: compared a signed size with `sizeof` (a negative size
  passed).
- Logging: what a peer can trigger logs at Warning or below (bad packets,
  unknown blocks, versions, codecs), so with AOO's default level (Error) a
  peer can't flood the log. That includes "jitter buffer not empty", which
  upstream calls a bug but which also happens when buffering gives up with
  blocks in the buffer (heavy loss, or a `/stop`).

Not done here (the glue's receive thread filters by peer, ID and format
first, see the spec): a cap on the sources a sink keeps (each new address
and ID from the network creates one), on the resends one request can ask
for, and on events queued while nobody polls. A forged `/stop` from a
player's address still ends that player's stream, as AOO's protocol says.

### Real-time safety

The plug-in calls `process()` of the source and the sinks on the host's
audio thread, which must not wait for a lock, the heap or the system.

- The queues the audio thread pushes to (the sink's resend requests and
  other requests per source, the source's requests, where a stopping stream
  queues a `/stop` per sink) took a new node from the heap whenever their
  free nodes ran out, as under loss. They now take nodes from AOO's
  lock-free RT memory pool (`rt_concurrent_queue` in `detail.hpp`) and
  reserve some up front.
- The sink's per-source event buffer (`event_buffer_`), a vector filled on
  the audio thread, had room for 6 events and grew on the heap past that
  (several overruns or stream time events in one call). It reserves 16 and
  grows from the RT pool.
- `get_random_id()` (stream and format IDs; the source's audio thread makes
  one per sink when a stream starts) built a static `std::random_device` and a
  `thread_local` engine on first use, from whichever thread got there
  first. It is now a lock-free counter, seeded in `aoo_initialize()` and
  mixed by a bijective hash: no repeats within 2^31 IDs, never negative.
- Stream messages that a sink schedules are allocated on the audio thread,
  from the RT pool, which uses the heap for blocks over 64 KB; one message
  could be that large. The cap on what a source can keep scheduled
  (`AOO_MAX_PENDING_STREAM_MESSAGES_SIZE`, see Hardening) is 16 KB.
- `make_new_stream()` (audio thread) cleared the source's queue of
  scheduled stream messages, which frees their heap memory; the send thread
  now does it, before it sends anything of the new stream.
- Events by polling only: `setEventHandler()` refuses
  `kAooEventModeCallback`, in which AOO calls the handler on the audio
  thread with its locks held.
- Logging: `AOO_LOG_LEVEL` defaults to Error (was Warning) and the new
  `aoo_setLogFunction()` replaces the log function at any time (the one in
  `AooSettings` only counts at the first `aoo_initialize()`), so that the
  glue can hand messages to another thread. AOO formats a message into a
  fixed buffer on the stack; the default function writes to stderr.

Not done here: the RT memory pool still falls back to the heap when it runs
out (1 MB by default) or for a block larger than `large_alloc_limit`
(64 KB); `live_rt_test` would count that.

### Opus

- `codec/opus.cpp`: the channels are paired into coupled Opus streams: a
  stereo signal is one coupled (joint stereo) stream, mono stays one plain
  stream, 3 channels are a coupled pair and a plain stream. Upstream sent
  one plain mono stream per channel ("what's the point of coupled
  streams?"): a stereo signal then spends the bitrate on two independent
  streams with no mid/side coding, and the L = R signal Jamsa sends for a
  mono input costs twice what it needs. **Our wire format is therefore no
  longer interoperable with upstream AOO peers**: the decoder must use the
  same layout, and nothing in `/start` says which one. Only Jamsa talks to
  Jamsa.
- `codec/aoo_opus.h`: `AooFormatOpus::bitrate` (bits per second,
  `OPUS_AUTO` by default, set by `AooFormatOpus_init()`). The encoder
  applies it in its setup, so the room's Live bitrate holds from the first
  packet on and survives a new `setFormat()`; `OPUS_SET_BITRATE` through
  `codecControl()` still changes it during a stream (the glue does that
  when the room's bitrate changes, without restarting the stream). The
  field is not serialized: `/start` is unchanged and the sink's format
  doesn't have it. A bitrate Opus refuses fails the setup.
- `codec/opus.cpp`, `OPUS_GET_BITRATE`: returns the bitrate last set
  (`OPUS_AUTO` if none). Upstream returned `OPUS_AUTO` always, because the
  multistream encoder's own getter was broken in Opus 1.3.2.

### Broadcast mode

The design's main saving: a player uploads its stream once, and our server
sends a copy to each listener, instead of uploading one copy per listener.
Upstream AOO can't do that: each sink gets its own stream ID and its own
copy of every data packet (`source.cpp`, `send_packet()`).

- `AooSource::setBroadcastEndpoint(ep)` (`kAooCtlSetBroadcastEndpoint`,
  one of our controls from `kAooCtlUserDefined` on; `nullptr` turns it
  off; `getBroadcast()` reads it). In broadcast mode every active sink gets
  the same stream ID, one per stream, made in `make_new_stream()`. A data
  packet is then the same for all those sinks (our source ID, the broadcast
  endpoint's sink ID, channel 0, the shared stream ID), and `send_packet()`
  sends it once, to the broadcast endpoint, instead of once per sink. The
  glue gives the endpoint peer ID 0, which our UDP header turns into "every
  other Live player in the room".
- `/start`, `/stop`, pings, pongs and resent data stay per sink (unicast),
  so a sink that lost a copy gets it again on its own, and one that
  receives data for a stream it doesn't know asks for `/start` as usual: a
  sink the source doesn't know yet gets nothing back until it is added,
  then joins the running stream without restarting it for the others.
- A sink the broadcast copy can't serve still gets its own copy: one with
  another sink ID, a channel offset, or a stream ID of its own (an
  accepted invitation, whose token becomes the stream ID).
- A sink that becomes active (`addSink()`, `activate()`) takes the shared
  stream ID. Both hold the source's reader lock until the sink has it, so
  `make_new_stream()`, which changes the ID under the writer lock on the
  audio thread, can't run in between and leave the new sink on the old
  stream. Turning broadcast mode on or off restarts a running stream.
