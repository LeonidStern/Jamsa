/*
  Round-trip test for opusencdec.h: encodes a continuous tone in several intervals,
  the way NJClient does, decodes each interval, and checks length, quality,
  and that consecutive intervals join without a jump.

  Build and run: make -C ninjam/tests opus_codec_test && ninjam/tests/opus_codec_test
*/

#include <stdio.h>
#include <math.h>
#include <vector>

#define VorbisEncoderInterface I_NJEncoder
#define VorbisDecoderInterface I_NJDecoder
#define WDL_VORBIS_INTERFACE_ONLY
#include "../../WDL/vorbisencdec.h"
#undef VorbisEncoderInterface
#undef VorbisDecoderInterface
#include "../opusencdec.h"

static int g_failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: " __VA_ARGS__); printf("\n"); g_failures++; } } while (0)

static const double FREQ = 997.0, AMP = 0.5, PI = 3.14159265358979;

// SNR of x (48 kHz) against the best-fitting tone at FREQ
static double tone_snr(const std::vector<float> &x, size_t start, size_t end)
{
  double ss = 0, sc = 0, cc = 0, sx = 0, cx = 0;
  for (size_t i = start; i < end; i ++)
  {
    const double s = sin(2*PI*FREQ*i/48000.0), c = cos(2*PI*FREQ*i/48000.0);
    ss += s*s; cc += c*c; sc += s*c; sx += s*x[i]; cx += c*x[i];
  }
  const double det = ss*cc - sc*sc;
  const double a = (sx*cc - cx*sc) / det, b = (cx*ss - sx*sc) / det;
  double sig = 0, err = 0;
  for (size_t i = start; i < end; i ++)
  {
    const double fit = a*sin(2*PI*FREQ*i/48000.0) + b*cos(2*PI*FREQ*i/48000.0);
    sig += fit*fit;
    err += (x[i]-fit)*(x[i]-fit);
  }
  return 10*log10(sig / (err + 1e-20));
}

static void run(int srate, int nch, int bitrate)
{
  printf("%d Hz, %d ch, %d kbps\n", srate, nch, bitrate);
  const int intervals = 4, interval_len = (int)(srate * 1.5), block = 512;

  NJOpusEncoder enc(srate, nch, bitrate, 1234);
  CHECK(!enc.isError(), "encoder error");

  std::vector<float> out; // channel 0 of everything decoded, 48 kHz
  std::vector<size_t> starts;
  std::vector<std::vector<float> > laps;
  long pos = 0, total_bytes = 0;
  std::vector<float> buf(block*2);

  for (int k = 0; k < intervals; k ++)
  {
    for (int done = 0; done < interval_len; done += block)
    {
      const int n = interval_len - done < block ? interval_len - done : block;
      for (int i = 0; i < n; i ++)
      {
        const float v = (float)(AMP*sin(2*PI*FREQ*(pos+i)/srate));
        buf[i] = v;
        buf[n+i] = v; // second channel, same as the first
      }
      pos += n;
      enc.Encode(buf.data(), n, 1, nch > 1 ? n : 0);
    }
    enc.Encode(NULL, 0);

    std::vector<unsigned char> stream;
    while (enc.Available() > 0)
    {
      const unsigned char *p = (const unsigned char *)enc.Get();
      stream.insert(stream.end(), p, p + enc.Available());
      enc.Advance(enc.Available());
    }
    enc.Compact();
    enc.reinit();
    total_bytes += (long)stream.size();
    CHECK(stream.size() > 4 && !memcmp(stream.data(), "OggS", 4), "interval %d is not an Ogg stream", k);

    NJOpusDecoder dec;
    for (size_t off = 0; off < stream.size(); off += 1000)
    {
      const int n = stream.size() - off < 1000 ? (int)(stream.size() - off) : 1000;
      memcpy(dec.DecodeGetSrcBuffer(n), stream.data() + off, n);
      dec.DecodeWrote(n);
    }
    CHECK(dec.GetSampleRate() == 48000 && dec.GetNumChannels() == nch, "decoder reports %d Hz %d ch", dec.GetSampleRate(), dec.GetNumChannels());

    const int frames = dec.Available() / dec.GetNumChannels();
    starts.push_back(out.size());
    for (int i = 0; i < frames; i ++) out.push_back(dec.Get()[i*nch]);
    if (nch > 1)
    {
      double diff = 0;
      for (int i = 0; i < frames; i ++) diff = fmax(diff, fabs(dec.Get()[i*2] - dec.Get()[i*2+1]));
      CHECK(diff < 0.05, "channels differ by %.3f", diff);
    }
    dec.Skip(dec.Available());

    const int lap = dec.GenerateLappingSamples();
    std::vector<float> l;
    for (int i = 0; i < lap; i ++) l.push_back(dec.Get()[i*nch]);
    laps.push_back(l);
    CHECK(lap >= 128, "only %d samples for the crossfade", lap);

    const double expect = interval_len * 48000.0 / srate;
    // the first interval is shorter by the resampler's delay
    CHECK(fabs(frames - expect) <= (k == 0 && srate != 48000 ? 64 : 2), "interval %d decoded to %d samples, expected %.0f", k, frames, expect);
  }

  printf("  %.1f kbps actual\n", total_bytes * 8.0 / (pos / (double)srate) / 1000.0);

  // the decoded tone is offset by the resampler's delay; fit it per interval
  for (int k = 0; k < intervals; k ++)
  {
    const size_t a = starts[k] + (k == 0 ? 2400 : 0), b = k+1 < intervals ? starts[k+1] : out.size();
    const double snr = tone_snr(out, a, b);
    printf("  interval %d: %.1f dB SNR\n", k, snr);
    CHECK(snr > 25.0, "interval %d SNR %.1f dB", k, snr);
  }

  // joins: the jump across each boundary should look like the tone's own sample-to-sample steps
  const double maxstep = 2*PI*FREQ/48000.0*AMP;
  for (int k = 1; k < intervals; k ++)
  {
    const size_t j = starts[k];
    const double jump = fabs(out[j] - out[j-1]);
    // NJClient crossfades the previous interval's padding into the first samples of the next
    double worst = 0;
    for (int i = 0; i < 128 && i < (int)laps[k-1].size(); i ++)
    {
      const double s = (i+1) / 129.0;
      const double mixed = out[j+i]*s + laps[k-1][i]*(1.0-s);
      const double ideal_step = fabs(mixed - (i ? out[j+i-1]*((i)/129.0) + laps[k-1][i-1]*(1.0-i/129.0) : out[j-1]));
      worst = fmax(worst, ideal_step);
    }
    printf("  join %d: jump %.4f, worst step in crossfade %.4f (tone steps up to %.4f)\n", k, jump, worst, maxstep);
    CHECK(jump < 3*maxstep, "join %d jumps by %.4f", k, jump);
    CHECK(worst < 3*maxstep, "crossfade at join %d steps by %.4f", k, worst);
  }
}

int main()
{
  run(48000, 1, 64);
  run(48000, 2, 85);
  run(44100, 1, 64);
  run(44100, 2, 85);
  printf(g_failures ? "%d failures\n" : "all passed\n", g_failures);
  return g_failures ? 1 : 0;
}
