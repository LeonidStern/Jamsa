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
own UDP socket and the packets that arrive back to AOO. That glue is
`liveclient.h` (the API the plug-in uses), `liveclient.cpp` (the socket and
the threads) and `livecore.h/.cpp` (everything else, testable without a
socket); see "The glue" below.

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

`CMakeLists.txt` builds the static library `ninjam_live` (AOO, oscpack and
the glue),
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
  with stream messages; and the heap calls a sink's `handleMessage()`
  makes while its `process()` isn't called. Also the other real-time
  changes below. `LIVE_RT_TRACE=1` prints where a
  counted call came from (glibc).
- `live_unit_test [test]`: what Live adds to AOO: coupled stereo Opus and
  its bitrate (encoded sizes, the decoder's channel layout, through the
  source too), broadcast mode with three sinks (one upload per block, a lost
  copy resent to its sink only, a sink that joins late, a channel offset,
  turning it on and off) and `removeSource()`. Then the glue's `Core`, with
  players on a virtual clock and network through a server that copies to
  the room as ours does (`live_sim.h`: a queue ordered by delivery time,
  delay, jitter and reordering, loss, loss bursts and duplicates in both
  directions, one seeded generator): the datagrams' layout and the server's
  in-place rewrite, a room of four (one upload per block each, tones heard
  at 50+ dB SNR), how late a player is heard against the statistics' estimate
  (within 3 ms, and the jitter buffer setting plus 2..15 ms), 7% loss
  resent in time, burst loss concealed by Opus without silent holes, 44.1,
  48 and 96 kHz in one room, 2048-sample host blocks (43 ms) at both ends,
  listeners with 1024..4096-sample host blocks and one at the 5 ms
  minimum, two clocks 200 ppm apart for five minutes
  (the delay holds within 2 ms, no dropouts; without dynamic resampling
  the buffer overruns), slot reuse with nine others for eight slots, a
  room of ten under the server's rate limit (each player hears all eight
  it has slots for, about 34 datagrams/s to single players), the
  filter in front of AOO verdict by verdict, fuzzed packets into a playing
  Core, the room's Live flag off and on, a host that stops calling for a
  second, a listener's host that stops for a minute while seven others
  play, and bitrate and frame size changes. About 25 s.
- `live_client_test`: the `Client` on real sockets: four players with
  audio threads paced by the clock and a minimal server on the loopback
  interface (HELLO / HELLO_ACK with the address cookie, DATA copied to the
  room). Binding within a round trip, everybody hears everybody (tones
  and bursts, at 44.1 and 48 kHz, with clocks 100 ppm off), the delay
  against the estimate, 5% lost copies resent, a forged HELLO_ACK and
  garbage ignored, the room's flag off and on, a player who leaves and one
  who takes the slot, stopping, and a restart (after `update()` with the
  UDP port closed, and `start()` while running) that keeps the players
  set before. About 25 s. With `-fsanitize=thread`, run
  it with `TSAN_OPTIONS=suppressions=ninjam/live/tests/tsan.supp`: AOO's
  lock-free free lists read a node another thread may be reusing, on
  purpose (see the file); the timing checks only print there.

## The glue

### Threads

- The engine's network thread (the "control thread") calls `start()`,
  `update()`, `setPeers()`, `setJitterMs()`, `stop()`, `poll()` (every
  ~30 ms: AOO's events become statistics and slot health) and `stats()`.
  The players set with `setPeers()` hold until `stop()`: a `start()`
  after a stop from `update()`, or while running, takes them up again.
- The audio thread calls `processBlock()` once per host block, then
  `slotOf()`, `slotHealthy()` and `stem()`. None of them waits (one
  try-lock against `prepare()`), allocates or logs; the only system call
  is the semaphore post that wakes the send thread, at most once per block
  and only when AOO has something to send and the send thread has run since
  the last post.
- The receive thread: `recv()` on the socket with a 100 ms timeout.
- The send thread, at a raised priority where the system allows it
  (`SCHED_FIFO` on Linux, which a plug-in usually can't get - then it keeps
  the normal one; the user-interactive QoS class on macOS;
  `THREAD_PRIORITY_TIME_CRITICAL` on Windows; `Counters` says which): wakes
  when the audio thread posts it, and every 10 ms, sends the HELLOs and
  whatever AOO has.

### Protocol (UDP)

All integers little-endian; datagrams over 1500 bytes are dropped. From a
player to the server (20-byte header):

    0  'N' 'L'   magic
    2  u8  version = 1
    3  u8  type      1 = HELLO, 2 = DATA
    4  u32 sender    our peer ID (from MESSAGE_SERVER_LIVE)
    8  u64 key       the connection's secret
    16 u32 dest      DATA: 0 = every other Live player in the room, else one
                     peer ID. HELLO: 0
    20 ...           HELLO: u64 time stamp (echoed), u64 cookie (the latest
                     one the server sent us, 0 for none), 4 zero bytes:
                     40 bytes in all. DATA: the AOO packet, 1..1452 bytes

From the server (8-byte header): magic, version, type (1 = HELLO_ACK,
2 = DATA), u32 from (DATA: the sender's peer ID; HELLO_ACK: 0), then
HELLO_ACK: the echoed time stamp and the cookie for the HELLO's source
address (24 bytes, smaller than the HELLO); DATA: the AOO packet. The
server forwards a DATA datagram by writing its header at offset 12 and
sending from there.

The client socket is `connect()`ed to the server, so the system drops
what comes from anywhere else. A HELLO goes out at start, every 2 s, and
at once when a HELLO_ACK brings a cookie other than the one we echoed (the
server binds our address only to a HELLO with a valid cookie, so binding
takes one round trip). We take a cookie only from an ACK that echoes one of
our last 8 HELLOs' time stamps (steady-clock microseconds), so a forged ACK
from our address can't change it; that ACK also gives the round trip to
the server (`serverRttMs()`).

### AOO's side

- One `AooSource` (ID 1) in broadcast mode: its data packets go once to
  peer 0, which our header turns into "the room". Opus,
  `OPUS_APPLICATION_RESTRICTED_LOWDELAY`, 48 kHz, 2 channels (a mono input
  is sent as L = R; coupled stereo costs little more), frames of the
  room's `frameSamples`, the room's bitrate (`update()` changes it during
  the stream; a new frame size restarts it).
- A fixed pool of `MAX_PEERS` (8) two-channel `AooSink`s (ID 1), created
  once; a slot is bound to a peer through an atomic, so the audio thread
  reads the mapping without a lock. `setPeers()` keeps the slots of those
  still there, frees the others (`removeSource()` on the slot's sink,
  statistics reset) and gives free slots to new players in the order
  given; a ninth player gets none and what it streams is dropped
  (`droppedPeer`).
- Every listed player (up to 64), with a slot or not, is an active sink of
  our source (`addSink()` when it joins the list, `removeSink()` when it
  leaves it): the server copies our stream to all of them, and a player
  who has a slot for us where we have none for it needs our `/start` and
  our answers. Before, only players with a slot were sinks: in a room of
  ten (`LiveMaxPeers` 10 or more; the default is 8), player 10 heard none
  of the eight it had slots for and asked each of them for a `/start`
  again and again, unanswered.
- AOO knows the players by made-up IPv4 addresses: peer N is `N.N.N.N`
  (`sin_addr = htonl(N)`), port 1, `addrlen` 16. The send callback turns
  them back into peer IDs.
- In front of AOO, for every DATA datagram (`Core::classify()`): the size;
  a sender with a slot (for our sinks) or in the list (for our source); the
  room's Live flag on; binary messages only
  `/data` between ID 1 and ID 1 in the short header; OSC only
  `/aoo/sink/1/{start,stop,data,ping,pong}` (to the slot's sink) and
  `/aoo/src/1/{start,stop,data,ping,pong}` (to our source; no invitations),
  first argument the other side's ID 1; a `/start` only for Opus, 1 or 2
  channels, 48 kHz, 120..480-sample blocks. AOO's own checks come after.
- The adapter: host blocks of any size go into AOO in blocks of 64 samples
  (`kAooFixedBlockSize`, which dynamic resampling needs; it is on at both
  ends). The stems are copied out of each sink's last block, so a stem lags
  by up to 64 samples. Each 64-sample block's time stamp is the host
  block's plus the samples before it (t + done / sr).
- The source's queue to the send thread holds two of the longest host
  blocks (`prepare()`'s `maxBlock`) and 10 ms, at least AOO's 25 ms: a host
  block goes in whole before the send thread sees any of it, and AOO drops
  what doesn't fit (2048-sample blocks lost 18 of every 43 ms). The jitter
  setting (the delay) is the caller's: with long blocks it needs about two
  of them and a few ms more (the plug-in raises it so,
  `Engine::liveBufferFor()`), as a player's audio comes a block at a time
  and plays a block at a time.
- A sink's jitter buffer holds the jitter setting plus two of the longest
  host blocks (`prepare()`'s `maxBlock`) and 5 ms, and never less than
  AOO's own twice the setting: a whole host block of packets goes into it
  at once, at the block's first 64 samples, and AOO sizes it for its own
  64-sample blocks. Before, 2048-sample host blocks at 20 ms, and the 5 ms
  minimum with any block, overflowed it again and again. The delay is the
  setting's, as before: the buffer only has more room.
- Time stamps come from the steady clock (`std::chrono::steady_clock`), not
  `aoo_getCurrentNtpTime()` (the wall clock): AOO only takes differences of
  them (its time DLLs, the sink's elapsed time), and a wall clock that
  steps (NTP, a user) would look like an xrun or warp the DLL. AOO's pings
  still use the wall clock, but only within one machine.
- When the host stops calling for more than 100 ms (processing suspended,
  a long xrun), the next block reports the gap (at most 0.5 s) to the
  source and the sinks as an xrun, so the source fills it with empty blocks
  and the sinks skip it.
- While the host isn't calling `processBlock()` (0.25 s after the end of
  the last block, or before the first one), the receive thread drops what
  comes for the sinks (`Counters::droppedIdle`): AOO would keep it until
  the next block and then work through all of it on the audio thread
  (with 7 players, 1.2 GB an hour and a 13 ms first block after a minute;
  see also `AOO_MAX_QUEUED_PACKETS`). What comes for our source still goes
  through, as the send thread runs on. The streams pick up from the next
  packets.
- `processBlock(..., send = false)` sends silence; the stream goes on.
- The room's Live flag off (`update()`): the source stops, every slot goes
  unhealthy at once and drops its source, and nothing reaches AOO until it
  is on again. HELLOs go on. `maySend` false: the source stops, we still
  listen.
- AOO logs through `aoo_setLogFunction()` into a small ring (try-lock: a
  message that would have to wait is counted and dropped); `poll()` writes
  it to stderr.

### Statistics and health

- `healthy`: the stream has been active for 1 s or more, with at most two
  dropouts (underruns, overruns, blocks lost for good) in the last 2 s, and
  the room's flag is on. The engine mixes a slot's stem instead of the
  player's intervals only while it is healthy.
- `oneWayMs`, how late we hear a player, without either audio interface:
  half the round trip AOO's pings measure, the encoder's and decoder's
  latency (frame, lookahead, resampling: AOO's `sourceLatency` and
  `sinkLatency`), our jitter buffer, and 64 samples for the two adapters.
  On a virtual network it is within 3 ms of the measured delay with host
  blocks of 64..128 samples; larger host blocks shift the jitter buffer's
  fill by up to about a block either way.
- `lossPct`: blocks lost on the way (resent or not) since the stream went
  active, over the blocks it has played since.
- A stream's first data packet can overtake its `/start` (network jitter);
  AOO then drops it and asks for `/start`, so one block at the start may be
  counted as dropped or resent before the slot is healthy.

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
- Sink jitter buffer, `add_packet()`: the gap between a new block and the
  newest one was taken in 32 bits. After a `/start` with sequence 0 (the
  newest is then -1), a block with sequence `INT32_MAX` overflowed it to a
  negative count, which passed the overrun check, and the gap was filled
  with 2^31 placeholder blocks on the audio thread (6 s per datagram in a
  release build, an assert in a debug one). It is now 64 bits.
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
- What a peer can make us send (`aoo_controls.h`, from `AOO_MAX_DATA_REQUESTS`
  on). Everything a player sends counts against its rate limit at our
  server, which leaves room for about one more stream on top of its own;
  a peer that used that room up would get the player's stream dropped for
  the whole room. One binary resend request could ask for about 180
  blocks, and the source resent all it still had; every ping got a pong,
  every `/start` request a `/start`, and a sink asked for `/start` once for
  each `/data` of a stream it didn't know (in broadcast mode, all of a
  room's data). Now a source reads at most 32 requests from a message (our
  sinks send no more in one); resends come out of a token bucket for all
  sinks together (half the stream's packets per second, bursts of 32) and
  one per sink (a quarter, bursts of 16), and what is over is dropped (the
  sink asks again while it can still use the block); pongs, `/start`s in
  answer to a request and a sink's `/start` requests go out at most once
  per 100 ms per peer. The clocks are the source's and the sink's elapsed
  time, so the virtual-time tests run them as the plug-in does.

Not done here (the glue's receive thread filters by peer, ID and format
first, see the spec): a cap on the sources a sink keeps (each new address
and ID from the network creates one) and on events queued while nobody
polls. A `/stop` request still gets a `/stop` back each time (only while
our stream is stopped or that sink inactive). A forged `/stop` from a
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
- The sink's packet queue per source (filled by `handleMessage()`, emptied
  only by `process()`) had no bound. While the host doesn't call
  `process()` (processing stopped, the plug-in still connected) every data
  packet took a frame from the heap and waited, and the next `process()`
  went through all of them on the audio thread (7 players for 2 minutes:
  +40 MB, never given back, and a 24..29 ms first block). It now keeps at
  most `AOO_MAX_QUEUED_PACKETS` (1024, over 2.5 s of the densest stream and
  6 times the largest jitter buffer) and drops newer ones; the jitter buffer
  catches up with the stream once `process()` runs again.

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

### Removing a source

- `AooSink::removeSource(ep)` (`kAooCtlRemoveSource`; `nullptr` = all):
  the glue keeps a fixed pool of sinks and gives a free one to the next
  peer. A sink only ever dropped a source after the source timeout (10 s
  by default) without packets, so the old peer's source would linger
  in the slot. The source is marked expired; the next `process()` drops it
  without playing it (the audio thread is the only one that removes
  sources) and sends `kAooEventSourceRemove`. Its packets, if they keep
  coming, add it again as a new source that asks for `/start`.
