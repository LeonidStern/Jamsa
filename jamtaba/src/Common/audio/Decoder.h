#ifndef _JTBA_AUDIO_DECODER_
#define _JTBA_AUDIO_DECODER_

#include <QByteArray>

#include "audio/core/SamplesBuffer.h"

/**
 * @brief The 'interface' for the decoders of ninjam intervals (Vorbis, Opus and FLAC)
 */
class AudioDecoder
{
    public:
        virtual ~AudioDecoder(){}

        virtual void setInputData(const QByteArray &encodedData) = 0;
        virtual void addInputData(const QByteArray &encodedData) = 0;
        virtual void setInputComplete() {} // all the interval data was added (voice chat intervals arrive in chunks)

        // decodes up to 'maxSamplesToDecode' samples. The returned buffer is stereo, and empty when nothing was decoded
        virtual const audio::SamplesBuffer &decode(int maxSamplesToDecode) = 0;

        virtual int getChannels() const = 0;
        virtual int getSampleRate() const = 0;
        virtual bool isFinished() const = 0; // all input was decoded
        virtual bool isValid() const = 0; // will be false when an error is detected
};

#endif
