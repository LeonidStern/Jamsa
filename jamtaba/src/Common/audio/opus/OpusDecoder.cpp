#include "OpusDecoder.h"
#include "NJOpus.h"

#include <cstring>

using opus::Decoder;

Decoder::Decoder() :
    decoder(new NJOpusDecoder()),
    inputPosition(0),
    inputComplete(false),
    internalBuffer(2, 4096)
{

}

Decoder::~Decoder()
{

}

void Decoder::setInputData(const QByteArray &opusData)
{
    input = opusData;
    inputPosition = 0;
}

void Decoder::addInputData(const QByteArray &opusData)
{
    if (inputPosition > 0 && inputPosition >= input.size() / 2) { // discard the bytes already fed to the decoder
        input.remove(0, inputPosition);
        inputPosition = 0;
    }

    input.append(opusData);
}

void Decoder::setInputComplete()
{
    inputComplete = true;
}

int Decoder::getChannels() const
{
    return decoder->GetNumChannels();
}

int Decoder::getSampleRate() const
{
    return decoder->GetSampleRate(); // 48 kHz, NinjamTrackNode resamples
}

bool Decoder::isFinished() const
{
    return inputComplete && inputPosition >= input.size() && decoder->Available() <= 0;
}

void Decoder::feedDecoder()
{
    // small pieces, so the decoder decodes one page at a time
    static const int PIECE_SIZE = 1024;

    while (decoder->Available() <= 0 && inputPosition < input.size()) {
        int bytes = qMin(PIECE_SIZE, input.size() - inputPosition);
        void *buffer = decoder->DecodeGetSrcBuffer(bytes);
        if (!buffer)
            return;

        std::memcpy(buffer, input.constData() + inputPosition, bytes);
        inputPosition += bytes;
        decoder->DecodeWrote(bytes);
    }
}

const audio::SamplesBuffer &Decoder::decode(int maxSamplesToDecode)
{
    feedDecoder();

    int channels = decoder->GetNumChannels();
    int samples = qMin(decoder->Available() / channels, maxSamplesToDecode);
    if (samples <= 0)
        return audio::SamplesBuffer::ZERO_BUFFER;

    // NJOpusDecoder output is interleaved, the internal buffer is always stereo
    internalBuffer.setFrameLenght(samples);
    const float *in = decoder->Get();
    float *left = internalBuffer.getSamplesArray(0);
    float *right = internalBuffer.getSamplesArray(1);
    int rightChannel = channels > 1 ? 1 : 0;
    for (int s = 0; s < samples; ++s) {
        left[s] = in[s * channels];
        right[s] = in[s * channels + rightChannel];
    }

    decoder->Skip(samples * channels);

    return internalBuffer;
}
