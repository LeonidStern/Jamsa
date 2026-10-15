#include "OpusEncoder.h"
#include "NJOpus.h"
#include "log/Logging.h"

#include <QRandomGenerator>
#include <QDebug>

using opus::Encoder;

int opus::bitrateForVorbisQuality(float quality)
{
    // njclient's table to choose a Vorbis quality for a bitrate (njclient.cpp), used backwards
    static const float qualities[] = { -0.1f, 0.0f, 0.1f, 0.3f };
    static const int bitrates[] = { 40, 64, 75, 95 };
    static const int points = 4;

    if (quality <= qualities[0])
        return bitrates[0];

    for (int i = 1; i < points; ++i) {
        if (quality <= qualities[i]) {
            float position = (quality - qualities[i - 1]) / (qualities[i] - qualities[i - 1]);
            return qRound(bitrates[i - 1] + position * (bitrates[i] - bitrates[i - 1]));
        }
    }

    return bitrates[points - 1];
}

Encoder::Encoder(uint channels, uint sampleRate, int bitrate) :
    encoder(new NJOpusEncoder(sampleRate, channels, bitrate, static_cast<int>(QRandomGenerator::global()->generate() & 0x7fffffff))),
    channels(channels),
    sampleRate(sampleRate),
    intervalFinished(false)
{
    qCDebug(jtNinjamCore) << "Initializing Opus Encoder sampleRate:" << sampleRate << " channels: " << channels << " bitrate: " << bitrate;

    if (encoder->isError())
        qCritical() << "opus encoder initialization error!";
}

Encoder::~Encoder()
{

}

QByteArray Encoder::takeEncodedData()
{
    int bytes = encoder->Available();
    QByteArray data(static_cast<const char *>(encoder->Get()), bytes);
    encoder->Advance(bytes);
    encoder->Compact();

    return data;
}

QByteArray Encoder::encode(const audio::SamplesBuffer &audioBuffer)
{
    if (encoder->isError())
        return QByteArray();

    if (intervalFinished) { // each interval is a new Ogg stream, reinit() writes the stream headers
        encoder->reinit();
        intervalFinished = false;
    }

    int samples = audioBuffer.getFrameLenght();
    int bufferChannels = audioBuffer.getChannels();
    if (samples > 0 && bufferChannels > 0) {
        interleavedSamples.resize(samples * channels);
        for (int c = 0; c < channels; ++c) {
            const float *in = audioBuffer.getSamplesArray(qMin(c, bufferChannels - 1));
            for (int s = 0; s < samples; ++s)
                interleavedSamples[s * channels + c] = in[s];
        }

        encoder->Encode(interleavedSamples.data(), samples, channels);
    }

    return takeEncodedData();
}

QByteArray Encoder::finishIntervalEncoding()
{
    if (encoder->isError())
        return QByteArray();

    encoder->Encode(nullptr, 0); // pads and closes the Ogg stream
    intervalFinished = true;

    return takeEncodedData();
}
