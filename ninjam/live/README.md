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
  (ported from AOO's own tests, with real checks instead of printouts).

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
