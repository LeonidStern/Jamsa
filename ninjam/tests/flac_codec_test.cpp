/*
  Round-trip test for flacencdec.h: encodes a continuous signal in several
  intervals, the way NJClient does, and decodes each interval from small,
  uneven pieces, the way it arrives from the network. FLAC is lossless, so the
  decoded audio must be the input to within the 16 bit rounding and dither
  (1.5 LSB), sample for sample, across every interval boundary, and silence
  must come back exactly silent.

  Also decodes a 24 bit stream (as another client might send), and checks that
  a stream cut short or garbage doesn't hang or crash the decoder.

  Build and run: make -C ninjam/tests flac_codec_test && ninjam/tests/flac_codec_test
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <vector>

#define VorbisEncoderInterface I_NJEncoder
#define VorbisDecoderInterface I_NJDecoder
#define WDL_VORBIS_INTERFACE_ONLY
#include "../../WDL/vorbisencdec.h"
#undef VorbisEncoderInterface
#undef VorbisDecoderInterface
#include "../flacencdec.h"

static int g_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); g_failures++; } } while (0)

static const double PI = 3.14159265358979;

static unsigned int g_rng = 12345;
static int rnd(int n) { g_rng = g_rng * 1103515245 + 12345; return (int)((g_rng >> 8) % (unsigned int)n); }

// a tone with some noise, so the encoder has real work to do, and a stretch of silence
static float signal(long i, int c, int srate)
{
  if (i % (srate * 2) < srate / 10) return 0.0f;
  return (float)(0.4 * sin(2*PI*(440.0 + 110.0*c)*i/srate) + 0.05 * (rnd(2001) - 1000) / 1000.0);
}

// feeds the stream in pieces of 1 to maxpiece bytes, taking the output as it comes
static std::vector<float> decode_in_pieces(NJFlacDecoder &dec, const std::vector<unsigned char> &stream, int maxpiece)
{
  std::vector<float> got;
  size_t off = 0;
  while (off < stream.size())
  {
    int n = 1 + rnd(maxpiece);
    if (n > (int)(stream.size() - off)) n = (int)(stream.size() - off);
    memcpy(dec.DecodeGetSrcBuffer(n), stream.data() + off, n);
    dec.DecodeWrote(n);
    off += n;
    if (rnd(3) == 0) dec.DecodeWrote(0); // NJClient polls when nothing new has arrived
    got.insert(got.end(), dec.Get(), dec.Get() + dec.Available());
    dec.Skip(dec.Available());
  }
  for (int i = 0; i < 3; i ++) dec.DecodeWrote(0);
  got.insert(got.end(), dec.Get(), dec.Get() + dec.Available());
  dec.Skip(dec.Available());
  return got;
}

static void run(int srate, int nch, int maxpiece)
{
  printf("%d Hz, %d ch, pieces up to %d bytes\n", srate, nch, maxpiece);
  const int intervals = 4, interval_len = (int)(srate * 1.3), block = 500;

  NJFlacEncoder enc(srate, nch, 64, 0);
  CHECK(!enc.isError(), "encoder error");

  std::vector<float> in, out; // interleaved
  long pos = 0, total_bytes = 0;
  std::vector<float> buf(block*2);

  for (int k = 0; k < intervals; k ++)
  {
    for (int done = 0; done < interval_len; done += block)
    {
      const int n = interval_len - done < block ? interval_len - done : block;
      // planar, as NJClient passes it: spacing n between the channels
      for (int i = 0; i < n; i ++)
        for (int c = 0; c < nch; c ++)
        {
          const float v = signal(pos+i, c, srate);
          buf[c*n + i] = v;
        }
      for (int i = 0; i < n; i ++)
        for (int c = 0; c < nch; c ++) in.push_back(buf[c*n + i]);
      pos += n;
      enc.Encode(buf.data(), n, 1, nch > 1 ? n : 0);
    }
    enc.Encode(NULL, 0);

    std::vector<unsigned char> stream;
    stream.insert(stream.end(), (unsigned char *)enc.Get(), (unsigned char *)enc.Get() + enc.Available());
    enc.Advance(enc.Available());
    enc.Compact();
    enc.reinit();
    total_bytes += (long)stream.size();
    CHECK(stream.size() > 4 && !memcmp(stream.data(), "fLaC", 4), "interval %d is not a FLAC stream", k);

    NJFlacDecoder dec;
    std::vector<float> got = decode_in_pieces(dec, stream, maxpiece);
    CHECK(dec.GetSampleRate() == srate && dec.GetNumChannels() == nch, "decoder reports %d Hz %d ch", dec.GetSampleRate(), dec.GetNumChannels());
    CHECK((int)got.size() == interval_len * nch, "interval %d decoded to %d samples, expected %d", k, (int)(got.size() / nch), interval_len);
    CHECK(dec.GenerateLappingSamples() == 0, "lossless intervals have no overlap");
    out.insert(out.end(), got.begin(), got.end());
  }

  printf("  %.0f kbps\n", total_bytes * 8.0 / (pos / (double)srate) / 1000.0);

  size_t mismatches = 0, first = 0, silent = 0, offgrid = 0;
  double sum = 0, sum2 = 0;
  for (size_t i = 0; i < in.size() && i < out.size(); i ++)
  {
    const double err = (out[i] - in[i]) * 32768.0;
    if (fabs(err) > 1.5 + 1e-3 && !mismatches++) first = i;
    if (in[i] == 0.0f)
    {
      silent ++;
      if (out[i] != 0.0f && !mismatches++) first = i;
    }
    if (out[i] * 32768.0f != floorf(out[i] * 32768.0f)) offgrid ++;
    sum += err;
    sum2 += err * err;
  }
  const double n = (double)(in.size() ? in.size() : 1);
  printf("  error %.3f LSB mean, %.3f LSB rms; %d silent samples\n", sum / n, sqrt(sum2 / n), (int)silent);
  CHECK(in.size() == out.size() && !mismatches, "%d of %d samples differ from the input by more than the dither (first at %d)", (int)mismatches, (int)in.size(), (int)first);
  CHECK(!offgrid, "%d samples are not 16 bit values", (int)offgrid);
  CHECK(fabs(sum / n) < 0.05 && sqrt(sum2 / n) < 0.75, "the error is not plain dither");
}

static void clipping()
{
  printf("clipping\n");
  NJFlacEncoder enc(48000, 1, 64, 0);
  float v[4] = { 2.0f, -2.0f, 1.0f, -1.0f };
  enc.Encode(v, 4);
  enc.Encode(NULL, 0);
  std::vector<unsigned char> stream((unsigned char *)enc.Get(), (unsigned char *)enc.Get() + enc.Available());
  NJFlacDecoder dec;
  std::vector<float> got = decode_in_pieces(dec, stream, 7);
  CHECK(got.size() == 4, "got %d samples", (int)got.size());
  if (got.size() == 4)
  {
    CHECK(got[0] > 0.9999f && got[1] == -1.0f && got[2] > 0.9999f && got[3] < -0.9999f, "clipped to %f %f %f %f", got[0], got[1], got[2], got[3]);
  }
}

static FLAC__StreamEncoderWriteStatus collect(const FLAC__StreamEncoder *, const FLAC__byte buffer[], size_t bytes, uint32_t, uint32_t, void *ctx)
{
  std::vector<unsigned char> *v = (std::vector<unsigned char> *)ctx;
  v->insert(v->end(), buffer, buffer + bytes);
  return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

// another client may send 24 bit (or any other depth): the decoder scales it to +-1
static void other_depth()
{
  printf("24 bit, 96 kHz stream\n");
  std::vector<unsigned char> stream;
  FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
  FLAC__stream_encoder_set_channels(e, 2);
  FLAC__stream_encoder_set_bits_per_sample(e, 24);
  FLAC__stream_encoder_set_sample_rate(e, 96000);
  FLAC__stream_encoder_set_compression_level(e, 8);
  CHECK(FLAC__stream_encoder_init_stream(e, collect, NULL, NULL, NULL, &stream) == FLAC__STREAM_ENCODER_INIT_STATUS_OK, "init");
  const int n = 50000;
  std::vector<FLAC__int32> l(n), r(n);
  for (int i = 0; i < n; i ++)
  {
    l[i] = (FLAC__int32)(4000000 * sin(2*PI*1000.0*i/96000.0));
    r[i] = -l[i];
  }
  const FLAC__int32 *ch[2] = { l.data(), r.data() };
  FLAC__stream_encoder_process(e, ch, n);
  FLAC__stream_encoder_finish(e);
  FLAC__stream_encoder_delete(e);

  NJFlacDecoder dec;
  std::vector<float> got = decode_in_pieces(dec, stream, 3000);
  CHECK(dec.GetSampleRate() == 96000 && dec.GetNumChannels() == 2, "decoder reports %d Hz %d ch", dec.GetSampleRate(), dec.GetNumChannels());
  CHECK((int)got.size() == 2*n, "decoded %d samples, expected %d", (int)got.size() / 2, n);
  double err = 0;
  for (int i = 0; i < n && 2*i+1 < (int)got.size(); i ++)
    err = fmax(err, fmax(fabs(got[2*i] - l[i] / 8388608.0), fabs(got[2*i+1] - r[i] / 8388608.0)));
  CHECK(err < 1e-6, "differs by %g", err);
}

static void damaged()
{
  printf("cut short, and garbage\n");
  NJFlacEncoder enc(44100, 2, 64, 0);
  std::vector<float> buf(2*20000);
  for (int i = 0; i < 20000; i ++) buf[i] = buf[20000+i] = signal(i, 0, 44100);
  enc.Encode(buf.data(), 20000, 1, 20000);
  enc.Encode(NULL, 0);
  std::vector<unsigned char> stream((unsigned char *)enc.Get(), (unsigned char *)enc.Get() + enc.Available());

  std::vector<unsigned char> cut(stream.begin(), stream.begin() + stream.size() * 2 / 3);
  NJFlacDecoder dec;
  std::vector<float> got = decode_in_pieces(dec, cut, 900);
  CHECK(got.size() > 0 && got.size() < 2*20000 && (got.size() / 2) % 4096 == 0, "cut stream decoded to %d samples", (int)got.size() / 2);

  std::vector<unsigned char> junk(30000);
  for (size_t i = 0; i < junk.size(); i ++) junk[i] = (unsigned char)rnd(256);
  NJFlacDecoder dec2;
  got = decode_in_pieces(dec2, junk, 2000);
  CHECK(got.empty(), "garbage decoded to %d samples", (int)got.size());

  std::vector<unsigned char> bad(stream);
  for (size_t i = 200; i < bad.size(); i += 97) bad[i] ^= 0x5a;
  NJFlacDecoder dec3;
  got = decode_in_pieces(dec3, bad, 1500); // no hang, no crash; what comes out doesn't matter
  printf("  damaged stream decoded to %d samples\n", (int)got.size() / 2);

  std::vector<unsigned char> one(stream);
  one[one.size() / 2] ^= 0x10; // one bad byte loses one frame; the decoder fills it with silence and goes on
  NJFlacDecoder dec5;
  got = decode_in_pieces(dec5, one, 700);
  CHECK(got.size() == 2*20000, "one bad byte: decoded to %d samples", (int)got.size() / 2);

  NJFlacDecoder dec4; // Reset() starts a new stream
  decode_in_pieces(dec4, cut, 500);
  dec4.Reset();
  got = decode_in_pieces(dec4, stream, 500);
  CHECK(got.size() == 2*20000, "after Reset() decoded to %d samples", (int)got.size() / 2);
}

int main()
{
  run(48000, 1, 1500);
  run(48000, 2, 1024);
  run(44100, 1, 300);
  run(44100, 2, 9000);
  run(96000, 2, 4000);
  clipping();
  other_depth();
  damaged();
  printf(g_failures ? "%d failures\n" : "all passed\n", g_failures);
  return g_failures ? 1 : 0;
}
