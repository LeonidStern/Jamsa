#include "FlacEncoder.h"
#include "log/Logging.h"

#include <QDebug>

#include <cmath>

using flac::Encoder;

Encoder::Encoder(uint channels, uint sampleRate) :
    encoder(FLAC__stream_encoder_new()),
    channels(qBound(1u, channels, 2u)),
    sampleRate(sampleRate),
    streamStarted(false),
    ditherState(0x12345678)
{
    qCDebug(jtNinjamCore) << "Initializing FLAC Encoder sampleRate:" << sampleRate << " channels: " << channels;

    if (!encoder)
        qCritical() << "FLAC encoder initialization error!";
}

Encoder::~Encoder()
{
    if (encoder)
        FLAC__stream_encoder_delete(encoder); // also finishes a started stream
}

bool Encoder::startStream()
{
    if (!encoder)
        return false;

    // FLAC__stream_encoder_finish() resets the settings, so they are set for every interval
    FLAC__stream_encoder_set_channels(encoder, channels);
    FLAC__stream_encoder_set_bits_per_sample(encoder, 16);
    FLAC__stream_encoder_set_sample_rate(encoder, sampleRate);
    FLAC__stream_encoder_set_compression_level(encoder, 5);

    // the 'fLaC' marker and the STREAMINFO are written now
    auto status = FLAC__stream_encoder_init_stream(encoder, writeCallback, nullptr, nullptr, nullptr, this);
    if (status != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        qCritical() << "FLAC encoder error:" << FLAC__StreamEncoderInitStatusString[status];
        return false;
    }

    streamStarted = true;
    return true;
}

FLAC__StreamEncoderWriteStatus Encoder::writeCallback(const FLAC__StreamEncoder *, const FLAC__byte buffer[],
                                                      size_t bytes, uint32_t, uint32_t, void *clientData)
{
    auto encoder = static_cast<Encoder *>(clientData);
    encoder->outBuffer.append(reinterpret_cast<const char *>(buffer), static_cast<qsizetype>(bytes));

    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

float Encoder::nextRandom()
{
    ditherState = ditherState * 1664525u + 1013904223u;
    return (ditherState >> 8) * (1.0f / 16777216.0f);
}

QByteArray Encoder::encode(const audio::SamplesBuffer &audioBuffer)
{
    outBuffer.clear();

    if (!streamStarted && !startStream())
        return QByteArray();

    int frames = audioBuffer.getFrameLenght();
    int bufferChannels = audioBuffer.getChannels();
    if (frames > 0 && bufferChannels > 0) {
        const FLAC__int32 *channelSamples[2];
        for (int c = 0; c < channels; ++c) {
            const float *in = audioBuffer.getSamplesArray(qMin(c, bufferChannels - 1));
            auto &out = samples[c];
            out.resize(frames);
            for (int s = 0; s < frames; ++s) {
                // TPDF dither of 1 LSB. Digital silence stays silent (and compresses to almost nothing)
                if (in[s] == 0.0f) {
                    out[s] = 0;
                    continue;
                }
                float dither = nextRandom() + nextRandom() - 1.0f;
                long value = std::lround(in[s] * 32768.0f + dither);
                out[s] = static_cast<FLAC__int32>(qBound(-32768L, value, 32767L));
            }
            channelSamples[c] = out.data();
        }

        if (!FLAC__stream_encoder_process(encoder, channelSamples, frames)) {
            auto state = FLAC__stream_encoder_get_state(encoder);
            qCritical() << "FLAC encoder error:" << FLAC__StreamEncoderStateString[state];
        }
    }

    return outBuffer;
}

QByteArray Encoder::finishIntervalEncoding()
{
    outBuffer.clear();

    if (streamStarted) {
        FLAC__stream_encoder_finish(encoder); // encodes the last (shorter) frame
        streamStarted = false;
    }

    return outBuffer;
}
