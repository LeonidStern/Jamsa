#include "FlacDecoder.h"
#include "log/Logging.h"

#include <QDebug>

#include <cmath>
#include <cstring>

using flac::Decoder;

Decoder::Decoder() :
    decoder(FLAC__stream_decoder_new()),
    inputPosition(0),
    inputOffset(0),
    inputComplete(false),
    metadataDecoded(false),
    endOfStream(false),
    valid(true),
    channels(1),
    sampleRate(44100),
    maxFrameSize(0),
    decodedPosition(0),
    internalBuffer(2, 4096)
{
    if (!decoder) {
        valid = false;
        return;
    }

    // no seek callback, intervals are always decoded from the start
    auto status = FLAC__stream_decoder_init_stream(decoder, readCallback, nullptr, tellCallback, nullptr, nullptr,
                                                   writeCallback, metadataCallback, errorCallback, this);
    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        qCritical() << "FLAC decoder error:" << FLAC__StreamDecoderInitStatusString[status];
        valid = false;
    }
}

Decoder::~Decoder()
{
    if (decoder)
        FLAC__stream_decoder_delete(decoder);
}

void Decoder::setInputData(const QByteArray &flacData)
{
    input = flacData;
    inputPosition = 0;
    inputOffset = 0;
}

void Decoder::addInputData(const QByteArray &flacData)
{
    if (metadataDecoded && inputPosition > 0 && inputPosition >= input.size() / 2) { // discard the bytes libFLAC already read
        input.remove(0, inputPosition);
        inputOffset += inputPosition;
        inputPosition = 0;
    }

    input.append(flacData);
}

void Decoder::setInputComplete()
{
    inputComplete = true;
}

bool Decoder::isFinished() const
{
    return (endOfStream || !valid) && decodedSamplesAvailable() == 0;
}

size_t Decoder::decodedSamplesAvailable() const
{
    return decodedSamples[0].size() - decodedPosition;
}

bool Decoder::metadataArrived() const
{
    if (inputComplete)
        return true;

    // the 'fLaC' marker and the metadata blocks, the last block has the 0x80 flag
    if (input.size() < 4 || std::memcmp(input.constData(), "fLaC", 4) != 0)
        return false; // not a FLAC stream (or not one JamTaba can decode while it arrives)

    const auto data = reinterpret_cast<const quint8 *>(input.constData());
    qint64 position = 4;
    while (position + 4 <= input.size()) {
        bool lastBlock = data[position] & 0x80;
        qint64 blockSize = (data[position + 1] << 16) | (data[position + 2] << 8) | data[position + 3];
        position += 4 + blockSize;
        if (lastBlock)
            return position <= input.size();
    }

    return false;
}

bool Decoder::nextFrameArrived() const
{
    if (inputComplete)
        return true;

    FLAC__uint64 decodedPosition = 0;
    if (maxFrameSize == 0 || !FLAC__stream_decoder_get_decode_position(decoder, &decodedPosition))
        return false;

    quint64 notDecoded = inputOffset + input.size() - decodedPosition;
    return notDecoded >= maxFrameSize;
}

bool Decoder::decodeNextBlock()
{
    if (!valid || endOfStream)
        return false;

    if (!metadataDecoded) {
        if (!metadataArrived())
            return false;

        if (!FLAC__stream_decoder_process_until_end_of_metadata(decoder)) {
            valid = false;
            return false;
        }

        metadataDecoded = true;
    }
    else {
        if (!nextFrameArrived())
            return false;

        if (!FLAC__stream_decoder_process_single(decoder)) {
            valid = false;
            return false;
        }
    }

    switch (FLAC__stream_decoder_get_state(decoder)) {
    case FLAC__STREAM_DECODER_SEARCH_FOR_METADATA:
    case FLAC__STREAM_DECODER_READ_METADATA:
    case FLAC__STREAM_DECODER_SEARCH_FOR_FRAME_SYNC:
    case FLAC__STREAM_DECODER_READ_FRAME:
        break;
    case FLAC__STREAM_DECODER_END_OF_STREAM:
        endOfStream = true;
        break;
    default: // aborted or errors
        valid = false;
    }

    return valid;
}

const audio::SamplesBuffer &Decoder::decode(int maxSamplesToDecode)
{
    while (decodedSamplesAvailable() == 0) {
        if (!decodeNextBlock())
            break;
    }

    int samples = static_cast<int>(qMin(decodedSamplesAvailable(), static_cast<size_t>(qMax(maxSamplesToDecode, 0))));
    if (samples <= 0)
        return audio::SamplesBuffer::ZERO_BUFFER;

    internalBuffer.setFrameLenght(samples);
    for (int c = 0; c < 2; ++c)
        std::memcpy(internalBuffer.getSamplesArray(c), decodedSamples[c].data() + decodedPosition, samples * sizeof(float));

    decodedPosition += samples;
    if (decodedPosition >= decodedSamples[0].size() / 2) { // discard the returned samples
        for (auto &channelSamples : decodedSamples)
            channelSamples.erase(channelSamples.begin(), channelSamples.begin() + decodedPosition);
        decodedPosition = 0;
    }

    return internalBuffer;
}

// ++++++++++++++++ libFLAC callbacks ++++++++++++++++++++++

FLAC__StreamDecoderReadStatus Decoder::readCallback(const FLAC__StreamDecoder *, FLAC__byte buffer[], size_t *bytes, void *clientData)
{
    auto instance = static_cast<Decoder *>(clientData);
    size_t available = instance->input.size() - instance->inputPosition;
    if (available == 0) {
        *bytes = 0;
        // waiting for more input can't happen, frames are decoded only after they arrive
        return instance->inputComplete ? FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM : FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }

    *bytes = qMin(*bytes, available);
    std::memcpy(buffer, instance->input.constData() + instance->inputPosition, *bytes);
    instance->inputPosition += static_cast<int>(*bytes);

    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderTellStatus Decoder::tellCallback(const FLAC__StreamDecoder *, FLAC__uint64 *absoluteByteOffset, void *clientData)
{
    // used by FLAC__stream_decoder_get_decode_position()
    auto instance = static_cast<Decoder *>(clientData);
    *absoluteByteOffset = instance->inputOffset + instance->inputPosition;

    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}

FLAC__StreamDecoderWriteStatus Decoder::writeCallback(const FLAC__StreamDecoder *, const FLAC__Frame *frame,
                                                      const FLAC__int32 *const buffer[], void *clientData)
{
    auto instance = static_cast<Decoder *>(clientData);
    const auto &header = frame->header;

    instance->channels = header.channels;
    instance->sampleRate = header.sample_rate;

    // any bit depth to float, mono is copied to both channels and only the first two channels are used
    const float scale = std::ldexp(1.0f, 1 - static_cast<int>(header.bits_per_sample));
    for (int c = 0; c < 2; ++c) {
        const FLAC__int32 *in = buffer[c < static_cast<int>(header.channels) ? c : 0];
        auto &out = instance->decodedSamples[c];
        size_t offset = out.size();
        out.resize(offset + header.blocksize);
        for (uint32_t s = 0; s < header.blocksize; ++s)
            out[offset + s] = in[s] * scale;
    }

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void Decoder::metadataCallback(const FLAC__StreamDecoder *, const FLAC__StreamMetadata *metadata, void *clientData)
{
    if (metadata->type != FLAC__METADATA_TYPE_STREAMINFO)
        return;

    auto instance = static_cast<Decoder *>(clientData);
    const auto &info = metadata->data.stream_info;

    instance->channels = info.channels;
    instance->sampleRate = info.sample_rate;

    // the largest frame, from the STREAMINFO or, when it's unknown, a frame with no compression
    instance->maxFrameSize = info.max_framesize;
    if (instance->maxFrameSize == 0)
        instance->maxFrameSize = static_cast<quint64>(info.max_blocksize) * info.channels * (info.bits_per_sample + 1) / 8 + 64;
}

void Decoder::errorCallback(const FLAC__StreamDecoder *, FLAC__StreamDecoderErrorStatus status, void *)
{
    // libFLAC skips the damaged frame and continues
    qCDebug(jtNinjamCore) << "FLAC decoder error:" << FLAC__StreamDecoderErrorStatusString[status];
}
