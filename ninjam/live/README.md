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
  `AOO_MAX_PENDING_STREAM_MESSAGES_SIZE`, 64 KB, per source).
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
