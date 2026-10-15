#ifndef NJOPUS_H
#define NJOPUS_H

/**
 * njclient's Ogg Opus encoder and decoder (ninjam/opusencdec.h), so JamTaba writes and reads
 * exactly the same 'OPUS' intervals as njclient. Include it only in .cpp files, the WDL headers
 * include windows.h.
 *
 * opusencdec.h implements njclient's I_NJEncoder and I_NJDecoder interfaces. njclient declares
 * them with WDL/vorbisencdec.h, which also pulls in LICE, so the same interfaces are declared here.
 */

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX // WDL includes windows.h, its min/max macros break std::min and std::max
#endif

class I_NJDecoder
{
public:
    virtual ~I_NJDecoder(){}
    virtual int GetSampleRate()=0;
    virtual int GetNumChannels()=0;
    virtual void *DecodeGetSrcBuffer(int srclen)=0;
    virtual void DecodeWrote(int srclen)=0;
    virtual void Reset()=0;
    virtual int Available()=0;
    virtual float *Get()=0;
    virtual void Skip(int amt)=0;
    virtual int GenerateLappingSamples()=0;
};

class I_NJEncoder
{
public:
    virtual ~I_NJEncoder(){}
    virtual void Encode(float *in, int inlen, int advance=1, int spacing=1)=0; // length in sample (PAIRS)
    virtual int isError()=0;
    virtual int Available()=0;
    virtual void *Get()=0;
    virtual void Advance(int)=0;
    virtual void Compact()=0;
    virtual void reinit(int bla=0)=0;
};

#include "opusencdec.h"

#endif // NJOPUS_H
