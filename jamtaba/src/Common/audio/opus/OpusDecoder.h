#ifndef OPUSDECODER_H
#define OPUSDECODER_H

#include "audio/core/SamplesBuffer.h"
#include "audio/Decoder.h"

#include <QByteArray>
#include <QScopedPointer>

class NJOpusDecoder;

namespace opus
{

/**
 * Decodes Ogg Opus intervals ('OPUS') with njclient's decoder (NJOpusDecoder). The decoded
 * audio is always 48 kHz.
 *
 * NJOpusDecoder decodes a whole Ogg page at a time (about half a second of audio), too slow
 * for the audio thread, so the intervals are decoded ahead in another thread (NinjamTrackNode).
 */
class Decoder : public AudioDecoder
{

public:
    Decoder();
    ~Decoder();

    void setInputData(const QByteArray &opusData) override;
    void addInputData(const QByteArray &opusData) override;
    void setInputComplete() override;

    const audio::SamplesBuffer &decode(int maxSamplesToDecode) override;

    int getChannels() const override;
    int getSampleRate() const override;
    bool isFinished() const override;
    bool isValid() const override { return true; } // damaged pages are skipped by NJOpusDecoder

private:
    QScopedPointer<NJOpusDecoder> decoder;
    QByteArray input;
    int inputPosition; // the next byte to feed into the decoder
    bool inputComplete;
    audio::SamplesBuffer internalBuffer;

    void feedDecoder(); // feeds the input until a page is decoded
};

} // namespace

#endif // OPUSDECODER_H
