#ifndef FLACDECODER_H
#define FLACDECODER_H

#include "audio/core/SamplesBuffer.h"
#include "audio/Decoder.h"

#include <FLAC/stream_decoder.h>

#include <QByteArray>

#include <vector>

namespace flac
{

/**
 * Decodes FLAC intervals ('FLAC') of any bit depth and sample rate.
 *
 * Voice chat intervals arrive in chunks, and libFLAC can't wait for more input in the middle of
 * a frame. So a frame is decoded only when the input is complete or when the bytes not decoded
 * yet are more than the largest frame of the stream.
 */
class Decoder : public AudioDecoder
{

public:
    Decoder();
    ~Decoder();

    void setInputData(const QByteArray &flacData) override;
    void addInputData(const QByteArray &flacData) override;
    void setInputComplete() override;

    const audio::SamplesBuffer &decode(int maxSamplesToDecode) override;

    int getChannels() const override;
    int getSampleRate() const override;
    bool isFinished() const override;
    bool isValid() const override { return valid; }

private:
    FLAC__StreamDecoder *decoder;

    QByteArray input;
    int inputPosition; // the next byte to give to libFLAC
    quint64 inputOffset; // stream position of the first byte in 'input'
    bool inputComplete;

    bool metadataDecoded;
    bool endOfStream;
    bool valid;

    int channels;
    int sampleRate;
    quint64 maxFrameSize; // in bytes, estimated from the STREAMINFO

    std::vector<float> decodedSamples[2]; // waiting to be returned by decode()
    size_t decodedPosition;
    audio::SamplesBuffer internalBuffer;

    bool decodeNextBlock(); // the metadata or the next frame
    bool metadataArrived() const;
    bool nextFrameArrived() const;
    size_t decodedSamplesAvailable() const;

    static FLAC__StreamDecoderReadStatus readCallback(const FLAC__StreamDecoder *decoder, FLAC__byte buffer[],
                                                      size_t *bytes, void *clientData);
    static FLAC__StreamDecoderTellStatus tellCallback(const FLAC__StreamDecoder *decoder, FLAC__uint64 *absoluteByteOffset,
                                                      void *clientData);
    static FLAC__StreamDecoderWriteStatus writeCallback(const FLAC__StreamDecoder *decoder, const FLAC__Frame *frame,
                                                        const FLAC__int32 *const buffer[], void *clientData);
    static void metadataCallback(const FLAC__StreamDecoder *decoder, const FLAC__StreamMetadata *metadata, void *clientData);
    static void errorCallback(const FLAC__StreamDecoder *decoder, FLAC__StreamDecoderErrorStatus status, void *clientData);
};

inline int Decoder::getChannels() const
{
    return channels;
}

inline int Decoder::getSampleRate() const
{
    return sampleRate;
}

} // namespace

#endif // FLACDECODER_H
