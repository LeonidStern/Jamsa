/*
    NINJAM - flacencdec.h
    Copyright (C) 2026 and onward, NINJAM contributors

    NINJAM is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/*

  FLAC encoder and decoder for NJClient, fourcc 'FLAC'. Each interval is one
  native FLAC stream ("fLaC", STREAMINFO, then frames; no Ogg), written as it
  is encoded, so the stream header never holds the total length.

  FLAC is lossless, so consecutive intervals join exactly and there is no
  overlap to crossfade. The encoder sends 16 bit samples (NJ_FLAC_BITS) at the
  client's sample rate, with TPDF dither of 1 LSB; digital silence stays exactly
  silent, so it compresses to almost nothing. The decoder plays any bit depth
  and rate.

  NJClient feeds the decoder whatever has arrived of an interval, and never
  says when the interval is complete. libFLAC can't decode from a partial frame
  and resume, so the decoder keeps the input itself: when libFLAC runs out in
  the middle of a frame, it stops it, rewinds to the end of the last whole frame
  and tries again once more has arrived.

  Needs libFLAC. Include after vorbisencdec.h, which defines the I_NJEncoder /
  I_NJDecoder interfaces in njclient.cpp.

*/

#ifndef _NJ_FLACENCDEC_H_
#define _NJ_FLACENCDEC_H_

#include <math.h>
#include <string.h>
#include <FLAC/stream_encoder.h>
#include <FLAC/stream_decoder.h>

#include "../WDL/queue.h"
#include "../WDL/heapbuf.h"

#ifndef NJ_FLAC_BITS
#define NJ_FLAC_BITS 16
#endif
#define NJ_FLAC_COMPRESSION 5


class NJFlacEncoder : public I_NJEncoder
{
public:
  NJFlacEncoder(int srate, int nch, int bitrate, int serno)
  {
    m_srate = srate;
    m_nch = nch;
    m_err = nch < 1 || nch > 2 || srate < 1;
    m_running = false;
    m_dither = 0x12345678u ^ (unsigned int)serno;
    m_enc = m_err ? NULL : FLAC__stream_encoder_new();
    if (!m_enc) m_err = 1;
    else startStream();
  }

  ~NJFlacEncoder()
  {
    if (m_enc)
    {
      if (m_running) FLAC__stream_encoder_finish(m_enc);
      FLAC__stream_encoder_delete(m_enc);
    }
  }

  void Encode(float *in, int inlen, int advance=1, int spacing=1) // length in sample (PAIRS)
  {
    if (m_err) return;
    if (!in || inlen <= 0)
    {
      // end of the interval: writes the last, shorter frame
      if (m_running) FLAC__stream_encoder_finish(m_enc);
      m_running = false;
      return;
    }
    if (!m_running) return; // needs reinit()

    const float scale = (float)(1 << (NJ_FLAC_BITS-1));
    const int maxv = (1 << (NJ_FLAC_BITS-1)) - 1, minv = -(1 << (NJ_FLAC_BITS-1));
    FLAC__int32 *buf = m_conv.Resize(inlen * m_nch, false);
    if (!buf) return;
    for (int c = 0; c < m_nch; c ++)
    {
      FLAC__int32 *out = buf + c * inlen;
      const float *src = in + c * spacing;
      for (int i = 0; i < inlen; i ++)
      {
        const float s = src[i * advance];
        if (s == 0.0f || s != s) // silence (and NaN) stays silent
        {
          out[i] = 0;
          continue;
        }
        const float f = s * scale + ditherRandom() + ditherRandom() - 1.0f; // TPDF dither, 1 LSB
        int v = (int)floor(f + 0.5f);
        if (v > maxv || f > (float)maxv) v = maxv;
        else if (v < minv || f < (float)minv) v = minv;
        out[i] = v;
      }
    }
    const FLAC__int32 *chans[2] = { buf, buf + (m_nch > 1 ? inlen : 0) };
    if (!FLAC__stream_encoder_process(m_enc, chans, (uint32_t)inlen))
    {
      m_err = 1;
      m_running = false;
    }
  }

  int isError() { return m_err; }

  int Available() { return m_out.Available(); }
  void *Get() { return m_out.Get(); }
  void Advance(int amt) { m_out.Advance(amt); }
  void Compact() { m_out.Compact(); }

  void reinit(int bla=0)
  {
    if (m_err) return;
    if (m_running) FLAC__stream_encoder_finish(m_enc);
    m_running = false;
    m_out.Advance(m_out.Available());
    m_out.Compact();
    startStream();
  }

private:
  bool configure(bool subset)
  {
    // libFLAC resets the settings when a stream finishes, so they are set for each one
    return FLAC__stream_encoder_set_channels(m_enc, m_nch) &&
           FLAC__stream_encoder_set_bits_per_sample(m_enc, NJ_FLAC_BITS) &&
           FLAC__stream_encoder_set_sample_rate(m_enc, m_srate) &&
           FLAC__stream_encoder_set_compression_level(m_enc, NJ_FLAC_COMPRESSION) &&
           FLAC__stream_encoder_set_streamable_subset(m_enc, subset) &&
           FLAC__stream_encoder_set_verify(m_enc, false);
  }

  void startStream()
  {
    for (int subset = 1; subset >= 0 && !m_running; subset --)
    {
      // unusual sample rates may be outside the streamable subset
      if (configure(!!subset) &&
          FLAC__stream_encoder_init_stream(m_enc, writeCallback, NULL, NULL, NULL, this) == FLAC__STREAM_ENCODER_INIT_STATUS_OK)
        m_running = true;
    }
    if (!m_running) m_err = 1;
  }

  float ditherRandom() // 0..1
  {
    m_dither = m_dither * 1664525u + 1013904223u;
    return (float)(m_dither >> 8) * (1.0f / 16777216.0f);
  }

  static FLAC__StreamEncoderWriteStatus writeCallback(const FLAC__StreamEncoder *, const FLAC__byte buffer[], size_t bytes,
                                                      uint32_t, uint32_t, void *client_data)
  {
    NJFlacEncoder *_this = (NJFlacEncoder *)client_data;
    _this->m_out.Add(buffer, (int)bytes);
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
  }

  FLAC__StreamEncoder *m_enc;
  int m_err, m_nch, m_srate;
  bool m_running;
  unsigned int m_dither;
  WDL_TypedBuf<FLAC__int32> m_conv;
  WDL_Queue m_out;
};


class NJFlacDecoder : public I_NJDecoder
{
public:
  NJFlacDecoder()
  {
    m_dec = FLAC__stream_decoder_new();
    if (m_dec) FLAC__stream_decoder_set_md5_checking(m_dec, false);
    m_inited = false;
    resetStream();
  }
  ~NJFlacDecoder()
  {
    if (m_dec)
    {
      if (m_inited) FLAC__stream_decoder_finish(m_dec);
      FLAC__stream_decoder_delete(m_dec);
    }
  }

  int GetSampleRate() { return m_srate > 0 ? m_srate : 48000; }
  int GetNumChannels() { return m_nch ? m_nch : 1; }

  void *DecodeGetSrcBuffer(int srclen) { return m_srcbuf.Resize(srclen > 0 ? srclen : 1, false); }

  void DecodeWrote(int srclen)
  {
    if (srclen > 0 && srclen <= m_srcbuf.GetSize()) m_in.Add(m_srcbuf.Get(), srclen);
    decode();
  }

  int Available() { return m_buf.Available(); }
  float *Get() { return m_buf.Get(); }

  void Skip(int amt)
  {
    m_buf.Advance(amt);
    m_buf.Compact();
  }

  int GenerateLappingSamples() { return 0; } // lossless: intervals join exactly

  void Reset()
  {
    m_buf.Clear();
    resetStream();
  }

private:
  FLAC__uint64 inputEnd() const { return m_base + (FLAC__uint64)m_in.Available(); }

  void resetStream()
  {
    if (m_dec && m_inited) FLAC__stream_decoder_finish(m_dec);
    m_inited = m_dec && FLAC__stream_decoder_init_stream(m_dec, readCallback, NULL, tellCallback, NULL, NULL,
                                                         writeCallback, metadataCallback, errorCallback, this) == FLAC__STREAM_DECODER_INIT_STATUS_OK;
    m_in.Clear();
    m_base = m_rd = m_good = 0;
    m_tried_at = (FLAC__uint64)-1;
    m_srate = m_nch = m_bps = 0;
    m_next_sample = 0;
    m_meta_done = false;
    m_bad = !m_inited;
  }

  // true once the stream's marker and all its metadata blocks have arrived
  bool haveMetadata()
  {
    const unsigned char *p = (const unsigned char *)m_in.Get();
    const int len = m_in.Available();
    if (len < 4) return false;
    if (memcmp(p, "fLaC", 4))
    {
      m_bad = true;
      return false;
    }
    int pos = 4;
    for (;;)
    {
      if (pos + 4 > len) return false;
      const bool last = (p[pos] & 0x80) != 0;
      pos += 4 + ((p[pos+1] << 16) | (p[pos+2] << 8) | p[pos+3]);
      if (pos > len) return false;
      if (last) return true;
    }
  }

  void decode()
  {
    if (m_bad) return;

    if (!m_meta_done)
    {
      if (!haveMetadata()) return;
      m_starved = false;
      if (!FLAC__stream_decoder_process_until_end_of_metadata(m_dec) || m_starved ||
          !FLAC__stream_decoder_get_decode_position(m_dec, &m_good))
      {
        m_bad = true;
        return;
      }
      m_meta_done = true;
    }

    for (;;)
    {
      const FLAC__uint64 end = inputEnd();
      if (m_good >= end || end == m_tried_at) return; // nothing new since the last try

      m_starved = false;
      const bool ok = !!FLAC__stream_decoder_process_single(m_dec);
      if (m_starved)
      {
        // the rest of the frame hasn't arrived: start it again later
        m_tried_at = end;
        FLAC__stream_decoder_flush(m_dec);
        m_rd = m_good;
        return;
      }
      FLAC__uint64 pos = 0;
      if (!ok || !FLAC__stream_decoder_get_decode_position(m_dec, &pos) || pos <= m_good)
      {
        m_bad = true;
        return;
      }
      m_good = pos;

      // drop input that libFLAC will never ask for again
      if (m_good > m_base && m_rd >= m_good)
      {
        m_in.Advance((int)(m_good - m_base));
        m_in.Compact();
        m_base = m_good;
      }
    }
  }

  static FLAC__StreamDecoderReadStatus readCallback(const FLAC__StreamDecoder *, FLAC__byte buffer[], size_t *bytes, void *client_data)
  {
    NJFlacDecoder *_this = (NJFlacDecoder *)client_data;
    const FLAC__uint64 end = _this->inputEnd();
    if (_this->m_rd >= end || _this->m_rd < _this->m_base)
    {
      *bytes = 0;
      _this->m_starved = true;
      return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }
    size_t n = (size_t)(end - _this->m_rd);
    if (n > *bytes) n = *bytes;
    memcpy(buffer, (const char *)_this->m_in.Get() + (size_t)(_this->m_rd - _this->m_base), n);
    _this->m_rd += n;
    *bytes = n;
    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
  }

  static FLAC__StreamDecoderTellStatus tellCallback(const FLAC__StreamDecoder *, FLAC__uint64 *absolute_byte_offset, void *client_data)
  {
    *absolute_byte_offset = ((NJFlacDecoder *)client_data)->m_rd;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
  }

  static FLAC__StreamDecoderWriteStatus writeCallback(const FLAC__StreamDecoder *, const FLAC__Frame *frame,
                                                      const FLAC__int32 * const buffer[], void *client_data)
  {
    NJFlacDecoder *_this = (NJFlacDecoder *)client_data;
    const int bps = frame->header.bits_per_sample ? (int)frame->header.bits_per_sample : _this->m_bps;
    if (!_this->m_nch || bps < 4 || bps > 32) return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
    if (!_this->m_srate) _this->m_srate = (int)frame->header.sample_rate;

    const int n = (int)frame->header.blocksize, fch = (int)frame->header.channels, nch = _this->m_nch;
    const float scale = 1.0f / (float)((FLAC__int64)1 << (bps-1));

    // a frame that was lost (damaged) becomes silence, so the rest of the interval stays in time
    if (frame->header.number_type == FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER)
    {
      const FLAC__uint64 first = frame->header.number.sample_number;
      if (first > _this->m_next_sample)
      {
        FLAC__uint64 gap = first - _this->m_next_sample, maxgap = (FLAC__uint64)_this->GetSampleRate() * 5;
        if (gap > maxgap) gap = maxgap;
        float *z = _this->m_buf.Add(NULL, (int)gap * nch);
        if (z) memset(z, 0, (size_t)gap * nch * sizeof(float));
      }
      _this->m_next_sample = first + n;
    }

    float *out = _this->m_buf.Add(NULL, n * nch);
    if (!out) return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
    for (int i = 0; i < n; i ++)
      for (int c = 0; c < nch; c ++)
        *out++ = c < fch ? (float)buffer[c][i] * scale : 0.0f;
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
  }

  static void metadataCallback(const FLAC__StreamDecoder *, const FLAC__StreamMetadata *metadata, void *client_data)
  {
    NJFlacDecoder *_this = (NJFlacDecoder *)client_data;
    if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO) return;
    _this->m_srate = (int)metadata->data.stream_info.sample_rate;
    _this->m_bps = (int)metadata->data.stream_info.bits_per_sample;
    const int nch = (int)metadata->data.stream_info.channels;
    _this->m_nch = nch > 2 ? 2 : nch; // NJClient plays mono or stereo
  }

  static void errorCallback(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus, void *) { }

  FLAC__StreamDecoder *m_dec;
  bool m_inited, m_meta_done, m_bad, m_starved;
  int m_srate, m_nch, m_bps;

  // m_in holds the input from byte m_base of the stream on; m_rd is the next byte
  // for libFLAC, m_good the end of the last frame it decoded
  WDL_Queue m_in;
  FLAC__uint64 m_base, m_rd, m_good, m_tried_at;
  FLAC__uint64 m_next_sample; // where the next frame should start

  WDL_HeapBuf m_srcbuf;
  WDL_TypedQueue<float> m_buf;
};

#endif
