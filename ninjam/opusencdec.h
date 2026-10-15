/*
    NINJAM - opusencdec.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  Ogg Opus (RFC 7845) encoder and decoder for NJClient, fourcc 'OPUS'.
  Each interval is its own Ogg stream, the same as Vorbis intervals.

  Opus always runs at 48 kHz. The encoder resamples other rates with a resampler
  that keeps running from one interval to the next, so interval boundaries stay
  seamless; the decoder always reports 48 kHz and NJClient resamples on playback.

  The end of each interval is padded with an LPC extrapolation of the audio
  instead of silence. The decoder doesn't play that padding (the stream's end
  trimming cuts it off) but hands it to NJClient's crossfade into the next
  interval through GenerateLappingSamples, like Vorbis hands over its overlap.

  Needs libopus and libogg. Include after vorbisencdec.h, which defines the
  I_NJEncoder / I_NJDecoder interfaces in njclient.cpp.

*/

#ifndef _NJ_OPUSENCDEC_H_
#define _NJ_OPUSENCDEC_H_

#include <math.h>
#include <string.h>
#include <opus/opus.h>
#include <ogg/ogg.h>

#include "../WDL/queue.h"
#include "../WDL/heapbuf.h"
#include "../WDL/resample.h"

#define NJ_OPUS_SRATE 48000
#define NJ_OPUS_FRAME 960 // 20 ms
#define NJ_OPUS_MAX_PACKET 1500
#define NJ_OPUS_LPC_ORDER 24
#define NJ_OPUS_LPC_HISTORY 1024 // samples per channel used to fit the extrapolation
#define NJ_OPUS_LAP_LEN 128 // padding kept at full level for the crossfade, then faded out

// fits an LPC predictor to data[0..n) (every stride'th value); from libvorbis' lpc.c (BSD)
static void nj_opus_lpc_from_data(const float *data, double *lpc, int n, int m, int stride)
{
  double aut[NJ_OPUS_LPC_ORDER+1];
  int i,j;
  for (j = m; j >= 0; j --)
  {
    double d = 0.0;
    for (i = j; i < n; i ++) d += (double)data[i*stride] * data[(i-j)*stride];
    aut[j] = d;
  }

  // lag window, so the fit doesn't ring on pure tones
  for (i = 1; i <= m; i ++)
  {
    const double w = 2.0 * 3.14159265358979 * 0.002 * i;
    aut[i] *= exp(-0.5 * w * w);
  }

  double error = aut[0] * (1.0 + 1e-10);
  const double epsilon = 1e-9 * aut[0] + 1e-10;
  for (i = 0; i < m; i ++)
  {
    double r = -aut[i+1];
    if (error < epsilon)
    {
      for (j = i; j < m; j ++) lpc[j] = 0.0;
      break;
    }
    for (j = 0; j < i; j ++) r -= lpc[j] * aut[i-j];
    r /= error;

    lpc[i] = r;
    for (j = 0; j < i/2; j ++)
    {
      const double tmp = lpc[j];
      lpc[j] += r * lpc[i-1-j];
      lpc[i-1-j] += r * tmp;
    }
    if (i & 1) lpc[j] += lpc[j] * r;

    error *= 1.0 - r * r;
  }

  // a little bandwidth expansion keeps the predictor stable
  double g = 0.999, damp = g;
  for (j = 0; j < m; j ++)
  {
    lpc[j] *= damp;
    damp *= g;
  }
}


class NJOpusEncoder : public I_NJEncoder
{
public:
  NJOpusEncoder(int srate, int nch, int bitrate, int serno) // bitrate in kbps
  {
    m_enc = NULL;
    m_nch = nch;
    m_srate = srate;
    m_ser = serno;
    m_use_rs = srate != NJ_OPUS_SRATE;
    m_finished = false;
    memset(&m_os,0,sizeof(m_os));
    m_os_init = false;

    int err = OPUS_OK;
    m_err = nch < 1 || nch > 2; // channel mapping family 0 is mono or stereo
    if (!m_err) m_enc = opus_encoder_create(NJ_OPUS_SRATE, nch, OPUS_APPLICATION_AUDIO, &err);
    if (!m_enc || err != OPUS_OK)
    {
      m_err = 1;
      return;
    }
    opus_encoder_ctl(m_enc, OPUS_SET_BITRATE(bitrate * 1000));
    // constrained VBR: stays near the bitrate asked for, so the server's bandwidth is predictable
    // (unconstrained VBR spends up to twice that on very tonal sounds)
    opus_encoder_ctl(m_enc, OPUS_SET_VBR(1));
    opus_encoder_ctl(m_enc, OPUS_SET_VBR_CONSTRAINT(1));
    opus_encoder_ctl(m_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(m_enc, OPUS_SET_COMPLEXITY(10));
    opus_int32 lookahead = 0;
    opus_encoder_ctl(m_enc, OPUS_GET_LOOKAHEAD(&lookahead));
    m_preskip = lookahead;

    if (m_use_rs)
    {
      m_rs.SetMode(false, 0, true); // sinc
      m_rs.SetRates(srate, NJ_OPUS_SRATE);
      m_rs.SetFeedMode(true);
    }

    startStream();
  }

  ~NJOpusEncoder()
  {
    if (m_enc) opus_encoder_destroy(m_enc);
    if (m_os_init) ogg_stream_clear(&m_os);
  }

  void Encode(float *in, int inlen, int advance=1, int spacing=1) // length in sample (PAIRS)
  {
    if (m_err) return;
    if (!in || inlen <= 0)
    {
      finishStream();
      return;
    }
    if (m_finished) return; // needs reinit()

    if (!m_use_rs)
    {
      float *out = m_pcm.Add(NULL, inlen * m_nch);
      float *hist = m_hist.Add(NULL, inlen * m_nch);
      if (!out || !hist) return;
      for (int i = 0; i < inlen; i ++)
        for (int c = 0; c < m_nch; c ++)
          *hist++ = *out++ = in[i*advance + c*spacing];
      m_real += inlen;
    }
    else
    {
      WDL_ResampleSample *rsin = NULL;
      const int need = m_rs.ResamplePrepare(inlen, m_nch, &rsin);
      if (!rsin) return;
      for (int i = 0; i < need; i ++)
        for (int c = 0; c < m_nch; c ++)
          *rsin++ = i < inlen ? in[i*advance + c*spacing] : 0.0f;

      const int maxout = (int)(need * (double)NJ_OPUS_SRATE / m_srate) + 64;
      WDL_ResampleSample *rsout = m_rsout.Resize(maxout * m_nch, false);
      const int got = m_rs.ResampleOut(rsout, need, maxout, m_nch);

      float *out = m_pcm.Add(NULL, got * m_nch);
      float *hist = m_hist.Add(NULL, got * m_nch);
      if (!out || !hist) return;
      for (int i = 0; i < got * m_nch; i ++) *hist++ = *out++ = (float)rsout[i];
      m_real += got;
    }

    trimHistory();
    encodeFrames(false);
  }

  int isError() { return m_err; }

  int Available() { return m_out.Available(); }
  void *Get() { return m_out.Get(); }
  void Advance(int amt) { m_out.Advance(amt); }
  void Compact() { m_out.Compact(); }

  void reinit(int bla=0)
  {
    if (m_err) return;
    m_out.Advance(m_out.Available());
    m_out.Compact();
    startStream();
  }

private:
  void startStream()
  {
    if (m_os_init) ogg_stream_clear(&m_os);
    ogg_stream_init(&m_os, m_ser++);
    m_os_init = true;
    opus_encoder_ctl(m_enc, OPUS_RESET_STATE);
    m_pcm.Clear();
    m_hist.Clear();
    m_real = 0;
    m_granule = 0;
    m_packetno = 0;
    m_finished = false;

    // OpusHead, RFC 7845 section 5.1
    unsigned char head[19];
    memcpy(head, "OpusHead", 8);
    head[8] = 1; // version
    head[9] = (unsigned char)m_nch;
    head[10] = m_preskip & 0xff;
    head[11] = (m_preskip >> 8) & 0xff;
    const unsigned int sr = (unsigned int)m_srate;
    head[12] = sr & 0xff;
    head[13] = (sr >> 8) & 0xff;
    head[14] = (sr >> 16) & 0xff;
    head[15] = (sr >> 24) & 0xff;
    head[16] = head[17] = 0; // output gain
    head[18] = 0; // channel mapping family 0
    writePacket(head, sizeof(head), 0, false, true);
    flushPages();

    // OpusTags, section 5.2
    static const char vendor[] = "NINJAM";
    unsigned char tags[8 + 4 + sizeof(vendor)-1 + 4];
    memcpy(tags, "OpusTags", 8);
    tags[8] = sizeof(vendor)-1;
    tags[9] = tags[10] = tags[11] = 0;
    memcpy(tags + 12, vendor, sizeof(vendor)-1);
    memset(tags + 12 + sizeof(vendor)-1, 0, 4); // no comments
    writePacket(tags, sizeof(tags), 0, false, false);
    flushPages();
  }

  void trimHistory()
  {
    const int keep = NJ_OPUS_LPC_HISTORY * m_nch;
    const int extra = m_hist.Available() - keep;
    if (extra > 0)
    {
      m_hist.Advance(extra);
      m_hist.Compact();
    }
  }

  // encodes all whole frames; at the end of the stream, also the padded last one
  void encodeFrames(bool final_frames)
  {
    const int framelen = NJ_OPUS_FRAME * m_nch;
    while (m_pcm.Available() >= framelen)
    {
      unsigned char pkt[NJ_OPUS_MAX_PACKET];
      const int len = opus_encode_float(m_enc, m_pcm.Get(), NJ_OPUS_FRAME, pkt, sizeof(pkt));
      m_pcm.Advance(framelen);
      if (len < 0)
      {
        m_err = 1;
        return;
      }
      m_granule += NJ_OPUS_FRAME;

      const bool last = final_frames && m_pcm.Available() < framelen;
      // the last packet's granule position marks where the real audio ends; the rest is padding
      writePacket(pkt, len, last ? m_real + m_preskip : m_granule, last, false);
    }
    m_pcm.Compact();
    if (final_frames) flushPages();
    else
    {
      ogg_page og;
      while (ogg_stream_pageout(&m_os, &og)) addPage(&og);
    }
  }

  void finishStream()
  {
    if (m_finished) return;
    m_finished = true;

    // pad so the encoder's lookahead is flushed and the last frame is whole
    const ogg_int64_t needed = m_real + m_preskip;
    const ogg_int64_t total = ((needed + NJ_OPUS_FRAME - 1) / NJ_OPUS_FRAME) * NJ_OPUS_FRAME;
    const int pad = (int)(total - m_real);

    float *p = m_pcm.Add(NULL, pad * m_nch);
    if (!p) return;
    memset(p, 0, pad * m_nch * sizeof(float));

    const int histlen = m_hist.Available() / m_nch;
    if (histlen > NJ_OPUS_LPC_ORDER * 2)
    {
      for (int c = 0; c < m_nch; c ++)
      {
        const float *h = m_hist.Get() + c;
        double lpc[NJ_OPUS_LPC_ORDER];
        nj_opus_lpc_from_data(h, lpc, histlen, NJ_OPUS_LPC_ORDER, m_nch);

        double mem[NJ_OPUS_LPC_ORDER]; // mem[k] = sample k+1 back
        for (int k = 0; k < NJ_OPUS_LPC_ORDER; k ++) mem[k] = h[(histlen-1-k) * m_nch];

        for (int i = 0; i < pad; i ++)
        {
          double y = 0.0;
          for (int k = 0; k < NJ_OPUS_LPC_ORDER; k ++) y -= lpc[k] * mem[k];
          memmove(mem + 1, mem, (NJ_OPUS_LPC_ORDER-1) * sizeof(double));
          mem[0] = y;

          // keep the start at full level for the crossfade, then fade out
          double w = 1.0;
          if (i >= NJ_OPUS_LAP_LEN && pad > NJ_OPUS_LAP_LEN)
          {
            const double t = (i - NJ_OPUS_LAP_LEN) / (double)(pad - NJ_OPUS_LAP_LEN);
            w = 0.5 + 0.5 * cos(3.14159265358979 * t);
          }
          p[i*m_nch + c] = (float)(y * w);
        }
      }
    }

    encodeFrames(true);
  }

  void writePacket(unsigned char *data, int len, ogg_int64_t granule, bool eos, bool bos)
  {
    ogg_packet op;
    op.packet = data;
    op.bytes = len;
    op.b_o_s = bos ? 1 : 0;
    op.e_o_s = eos ? 1 : 0;
    op.granulepos = granule;
    op.packetno = m_packetno++;
    ogg_stream_packetin(&m_os, &op);
  }

  void flushPages()
  {
    ogg_page og;
    while (ogg_stream_flush(&m_os, &og)) addPage(&og);
  }

  void addPage(ogg_page *og)
  {
    m_out.Add(og->header, og->header_len);
    m_out.Add(og->body, og->body_len);
  }

  OpusEncoder *m_enc;
  int m_err, m_nch, m_srate, m_ser, m_preskip;
  bool m_use_rs, m_finished, m_os_init;
  ogg_stream_state m_os;
  ogg_int64_t m_real, m_granule, m_packetno; // m_real: samples at 48 kHz this stream, without padding

  WDL_Resampler m_rs;
  WDL_TypedBuf<WDL_ResampleSample> m_rsout;
  WDL_TypedQueue<float> m_pcm; // interleaved 48 kHz audio waiting to fill a frame
  WDL_TypedQueue<float> m_hist; // the last NJ_OPUS_LPC_HISTORY samples, for the end padding
  WDL_Queue m_out;
};


class NJOpusDecoder : public I_NJDecoder
{
public:
  NJOpusDecoder()
  {
    m_dec = NULL;
    m_nch = 0;
    memset(&m_os,0,sizeof(m_os));
    m_os_init = false;
    ogg_sync_init(&m_oy);
    resetStream();
  }
  ~NJOpusDecoder()
  {
    if (m_dec) opus_decoder_destroy(m_dec);
    if (m_os_init) ogg_stream_clear(&m_os);
    ogg_sync_clear(&m_oy);
  }

  int GetSampleRate() { return NJ_OPUS_SRATE; }
  int GetNumChannels() { return m_nch ? m_nch : 1; }

  void *DecodeGetSrcBuffer(int srclen) { return ogg_sync_buffer(&m_oy, srclen); }

  void DecodeWrote(int srclen)
  {
    ogg_sync_wrote(&m_oy, srclen);

    ogg_page og;
    while (ogg_sync_pageout(&m_oy, &og) > 0)
    {
      const int serial = ogg_page_serialno(&og);
      if (!m_os_init || serial != m_os.serialno)
      {
        resetStream();
        if (m_os_init) ogg_stream_clear(&m_os);
        ogg_stream_init(&m_os, serial);
        m_os_init = true;
      }
      if (ogg_stream_pagein(&m_os, &og) < 0) continue;

      // decode the whole page first, so the padding at the end of the stream can be split off
      m_page.Clear();
      ogg_packet op;
      while (ogg_stream_packetout(&m_os, &op) > 0)
      {
        if (m_packets == 0)
        {
          if (!parseHead(op.packet, op.bytes)) m_bad = true;
        }
        else if (m_packets >= 2 && m_dec && !m_bad)
        {
          float *tmp = m_tmp;
          const int n = opus_decode_float(m_dec, op.packet, (opus_int32)op.bytes, tmp, 5760, 0);
          if (n > 0)
          {
            m_decoded += n;
            int skip = m_preskip_left < n ? m_preskip_left : n;
            m_preskip_left -= skip;
            float *out = m_page.Add(NULL, (n - skip) * m_nch);
            if (out) for (int i = skip * m_nch; i < n * m_nch; i ++) *out++ = tmp[i] * m_gain;
          }
        }
        m_packets ++;
      }

      int keep = m_page.Available() / (m_nch ? m_nch : 1);
      if (ogg_page_eos(&og) && m_dec)
      {
        const ogg_int64_t end = ogg_page_granulepos(&og);
        if (end >= 0 && m_decoded > end)
        {
          ogg_int64_t extra = m_decoded - end;
          if (extra > keep) extra = keep;
          keep -= (int)extra;
          m_lap.Clear();
          m_lap.Add(m_page.Get() + keep * m_nch, (int)extra * m_nch);
        }
      }
      if (keep > 0) m_buf.Add(m_page.Get(), keep * m_nch);
    }
  }

  int Available() { return m_buf.Available(); }
  float *Get() { return m_buf.Get(); }

  void Skip(int amt)
  {
    m_buf.Advance(amt);
    m_buf.Compact();
  }

  int GenerateLappingSamples()
  {
    const int n = m_nch ? m_lap.Available() / m_nch : 0;
    if (n > 0) m_buf.Add(m_lap.Get(), n * m_nch);
    m_lap.Clear();
    return n;
  }

  void Reset()
  {
    m_buf.Clear();
    resetStream();
    if (m_os_init) ogg_stream_clear(&m_os);
    memset(&m_os,0,sizeof(m_os));
    m_os_init = false;
  }

private:
  void resetStream()
  {
    if (m_dec) opus_decoder_destroy(m_dec);
    m_dec = NULL;
    m_packets = 0;
    m_decoded = 0;
    m_preskip_left = 0;
    m_gain = 1.0f;
    m_bad = false;
    m_lap.Clear();
  }

  bool parseHead(const unsigned char *p, long len)
  {
    if (len < 19 || memcmp(p, "OpusHead", 8) || (p[8] & 0xf0)) return false;
    const int nch = p[9];
    const int family = p[18];
    if (family != 0 || nch < 1 || nch > 2) return false; // mono/stereo only
    m_preskip_left = p[10] | (p[11] << 8);
    const short gain = (short)(p[16] | (p[17] << 8));
    m_gain = gain ? (float)pow(10.0, gain / (20.0 * 256.0)) : 1.0f;

    int err = OPUS_OK;
    m_dec = opus_decoder_create(NJ_OPUS_SRATE, nch, &err);
    if (!m_dec || err != OPUS_OK)
    {
      if (m_dec) opus_decoder_destroy(m_dec);
      m_dec = NULL;
      return false;
    }
    m_nch = nch;
    return true;
  }

  OpusDecoder *m_dec;
  int m_nch, m_packets, m_preskip_left;
  float m_gain;
  bool m_bad, m_os_init;
  ogg_int64_t m_decoded; // samples decoded this stream, including the pre-skip

  ogg_sync_state m_oy;
  ogg_stream_state m_os;

  WDL_TypedQueue<float> m_buf, m_lap, m_page;
  float m_tmp[5760*2]; // longest Opus packet (120 ms), stereo
};

#endif
