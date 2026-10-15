#ifndef FLACENCODER_H
#define FLACENCODER_H

#include "audio/core/SamplesBuffer.h"
#include "audio/Encoder.h"

#include <FLAC/stream_encoder.h>

#include <QByteArray>

#include <vector>

namespace flac
{

/**
 * Encodes each interval as a FLAC stream ('FLAC'): 16 bits with TPDF dither, at the session sample rate.
 */
class Encoder : public AudioEncoder
{

public:
    Encoder(uint channels, uint sampleRate);
    ~Encoder();

    QByteArray encode(const audio::SamplesBuffer &audioBuffer) override;
    QByteArray finishIntervalEncoding() override;

    int getChannels() const override;
    int getSampleRate() const override;
    ninjam::AudioCodec getCodec() const override { return ninjam::AudioCodec::Flac; }

private:
    FLAC__StreamEncoder *encoder;
    int channels;
    int sampleRate;
    bool streamStarted;
    QByteArray outBuffer;
    std::vector<FLAC__int32> samples[2];
    quint32 ditherState;

    bool startStream();
    float nextRandom(); // 0 to 1

    static FLAC__StreamEncoderWriteStatus writeCallback(const FLAC__StreamEncoder *encoder, const FLAC__byte buffer[],
                                                        size_t bytes, uint32_t samples, uint32_t currentFrame, void *clientData);
};

inline int Encoder::getChannels() const
{
    return channels;
}

inline int Encoder::getSampleRate() const
{
    return sampleRate;
}

} // namespace

#endif // FLACENCODER_H
