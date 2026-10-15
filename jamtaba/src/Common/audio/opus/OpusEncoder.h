#ifndef OPUSENCODER_H
#define OPUSENCODER_H

#include "audio/core/SamplesBuffer.h"
#include "audio/Encoder.h"

#include <QByteArray>
#include <QScopedPointer>

#include <vector>

class NJOpusEncoder;

namespace opus
{

// njclient sends Opus at the bitrate set for Vorbis, so JamTaba uses the bitrates matching its Vorbis qualities
int bitrateForVorbisQuality(float quality); // in kbps

/**
 * Encodes each interval as an Ogg Opus stream ('OPUS'), using njclient's encoder (NJOpusEncoder).
 */
class Encoder : public AudioEncoder
{

public:
    Encoder(uint channels, uint sampleRate, int bitrate); // bitrate in kbps
    ~Encoder();

    QByteArray encode(const audio::SamplesBuffer &audioBuffer) override;
    QByteArray finishIntervalEncoding() override;

    int getChannels() const override;
    int getSampleRate() const override;
    ninjam::AudioCodec getCodec() const override { return ninjam::AudioCodec::Opus; }

private:
    QScopedPointer<NJOpusEncoder> encoder;
    int channels;
    int sampleRate;
    bool intervalFinished; // a new Ogg stream is started in the next encode()
    std::vector<float> interleavedSamples;

    QByteArray takeEncodedData();
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

#endif // OPUSENCODER_H
