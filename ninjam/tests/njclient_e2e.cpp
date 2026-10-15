/*
  Two NJClients in one process, connected to a running server: "sender" plays a
  997 Hz tone into its channel, "listener" subscribes and records what it hears.
  Prints the listener's SNR for the tone over the last intervals, which shows
  whether audio made it through the encoder, the server and the decoder.

  Usage: njclient_e2e host:port seconds samplerate
  Used by test_opus_e2e.py.
*/

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <vector>

#include "../njclient.h"

static const double FREQ = 997.0, PI = 3.14159265358979;

static bool run_until_connected(NJClient &a, NJClient &b)
{
  for (int i = 0; i < 1000; i ++)
  {
    a.Run();
    b.Run();
    if (a.GetStatus() == NJClient::NJC_STATUS_OK && b.GetStatus() == NJClient::NJC_STATUS_OK && a.GetBPI() > 0) return true;
    if (a.GetStatus() < 0 || b.GetStatus() < 0) return false;
    usleep(5000);
  }
  return false;
}

static double tone_snr(const std::vector<float> &x, size_t start, size_t end, int srate)
{
  double ss = 0, sc = 0, cc = 0, sx = 0, cx = 0;
  for (size_t i = start; i < end; i ++)
  {
    const double s = sin(2*PI*FREQ*i/srate), c = cos(2*PI*FREQ*i/srate);
    ss += s*s; cc += c*c; sc += s*c; sx += s*x[i]; cx += c*x[i];
  }
  const double det = ss*cc - sc*sc;
  const double a = (sx*cc - cx*sc) / det, b = (cx*ss - sx*sc) / det;
  double sig = 0, err = 0;
  for (size_t i = start; i < end; i ++)
  {
    const double fit = a*sin(2*PI*FREQ*i/srate) + b*cos(2*PI*FREQ*i/srate);
    sig += fit*fit;
    err += (x[i]-fit)*(x[i]-fit);
  }
  return sig > 0 ? 10*log10(sig / (err + 1e-20)) : -100.0;
}

int main(int argc, char **argv)
{
  if (argc < 4)
  {
    fprintf(stderr, "usage: %s host:port seconds samplerate\n", argv[0]);
    return 2;
  }
  const double seconds = atof(argv[2]);
  const int srate = atoi(argv[3]);
  const int block = 512;

  JNL::open_socketlib();
  NJClient sender, listener;
  for (NJClient *c : { &sender, &listener })
  {
    c->config_metronome_mute = true;
    c->config_metronome = 0.0f;
  }
  sender.SetLocalChannelInfo(0, "tone", true, 0, true, 64, true, true);

  sender.Connect(argv[1], "anonymous:sender", "");
  listener.Connect(argv[1], "anonymous:listener", "");
  if (!run_until_connected(sender, listener))
  {
    printf("error: could not connect (status %d / %d)\n", sender.GetStatus(), listener.GetStatus());
    return 1;
  }

  std::vector<float> in0(block), in1(block), out0(block), out1(block), silent(block);
  std::vector<float> heard;
  long pos = 0;
  const long total = (long)(seconds * srate);
  while (pos < total)
  {
    for (int i = 0; i < block; i ++) in0[i] = in1[i] = (float)(0.5 * sin(2*PI*FREQ*(pos+i)/srate));
    float *sin_[2] = { in0.data(), in1.data() }, *sout[2] = { out0.data(), out1.data() };
    sender.AudioProc(sin_, 2, sout, 2, block, srate);

    float *lin[2] = { silent.data(), silent.data() }, *lout[2] = { out0.data(), out1.data() };
    listener.AudioProc(lin, 2, lout, 2, block, srate);
    heard.insert(heard.end(), out0.begin(), out0.end());
    pos += block;

    for (int i = 0; i < 4; i ++)
    {
      sender.Run();
      listener.Run();
    }
    usleep((useconds_t)(1000000.0 * block / srate / 4)); // about 4x real time, so the network keeps up
  }

  int ilen = 0, bpi = sender.GetBPI();
  sender.GetPosition(NULL, &ilen);
  // NINJAM plays what others sent one interval late; measure the last two intervals
  const size_t end = heard.size(), start = end > (size_t)ilen * 2 ? end - (size_t)ilen * 2 : 0;
  double peak = 0;
  for (size_t i = start; i < end; i ++) peak = fmax(peak, fabs(heard[i]));
  printf("bpi=%d interval_samples=%d peak=%.3f snr=%.1f\n", bpi, ilen, peak, tone_snr(heard, start, end, srate));

  sender.Disconnect();
  listener.Disconnect();
  return 0;
}
