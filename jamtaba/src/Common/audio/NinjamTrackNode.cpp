#include "NinjamTrackNode.h"

#include <QDataStream>
#include <QDebug>
#include <QList>
#include <QByteArray>
#include <QMutexLocker>
#include <QDateTime>
#include <QThreadPool>

#include "audio/core/Filters.h"
#include "audio/core/AudioDriver.h"
#include "audio/vorbis/VorbisDecoder.h"
#include "audio/opus/OpusDecoder.h"
#include "audio/flac/FlacDecoder.h"

#include <atomic>


const double NinjamTrackNode::LOW_CUT_DRASTIC_FREQUENCY = 220.0; // in Hertz
const double NinjamTrackNode::LOW_CUT_NORMAL_FREQUENCY = 120.0; // in Hertz

using audio::Filter;

class NinjamTrackNode::LowCutFilter
{
public:
    explicit LowCutFilter(double sampleRate);
    void process(audio::SamplesBuffer &buffer);
    //inline bool isActivated() const { return activated; }
    inline NinjamTrackNode::LowCutState getState(){ return this->state; }
    void setState(NinjamTrackNode::LowCutState state);
private:
    NinjamTrackNode::LowCutState state;
    Filter leftFilter;
    Filter rightFilter;
};

NinjamTrackNode::LowCutFilter::LowCutFilter(double sampleRate) :
    state(LowCutState::Off),
    leftFilter(Filter::FilterType::HighPass, sampleRate, LOW_CUT_NORMAL_FREQUENCY, 1.0, 1.0),
    rightFilter(Filter::FilterType::HighPass, sampleRate, LOW_CUT_NORMAL_FREQUENCY, 1.0, 1.0)
{

}

void NinjamTrackNode::LowCutFilter::setState(NinjamTrackNode::LowCutState state)
{
    this->state = state;
    if (state != LowCutState::Off){
        double frequency = LOW_CUT_NORMAL_FREQUENCY;
        if (state == LowCutState::Drastic)
            frequency = LOW_CUT_DRASTIC_FREQUENCY;

        leftFilter.setFrequency(frequency);
        rightFilter.setFrequency(frequency);
    }
}

void NinjamTrackNode::LowCutFilter::process(audio::SamplesBuffer &buffer)
{
    if (state == LowCutState::Off)
        return;

    quint32 samples = buffer.getFrameLenght();
    leftFilter.process(buffer.getSamplesArray(0), samples);
    if (!buffer.isMono())
        rightFilter.process(buffer.getSamplesArray(1), samples);
}

//--------------------------------------------------------------------------
NinjamTrackNode::TrackNodeCommand::TrackNodeCommand(NinjamTrackNode *node)
    : trackNode(node)
{

}

class NinjamTrackNode::ChangeChannelModeCommand : public NinjamTrackNode::TrackNodeCommand
{
private:
    ChannelMode newChannelMode;

public:
    ChangeChannelModeCommand(NinjamTrackNode *node, ChannelMode newChannelMode)
        : NinjamTrackNode::TrackNodeCommand(node),
          newChannelMode(newChannelMode)
    {
        //
    }

    void execute() override {
        trackNode->setChannelMode(newChannelMode);
    }
};

//--------------------------------------------------------------------------

class NinjamTrackNode::IntervalDecoder
{
public:
    IntervalDecoder(ninjam::AudioCodec codec, bool isVoiceChat);
    void decodeAhead(); // called in a worker thread
    bool needsDecodingAhead();
    void addEncodedData(const QByteArray &encodedData, bool isLastPart);
    quint32 getDecodedSamples(audio::SamplesBuffer &outBuffer, uint samplesToDecode, uint outOffset = 0);
    inline int getSampleRate() const { return decoder->getSampleRate(); }
    inline bool isStereo() const { return decoder->getChannels() == 2; }
    void stopDecoding();
    bool isFullyDecoded();
    bool isValid() const { return decoder->isValid(); }
    inline void skipBuffering() { playing = true; } // voice chat: the previous interval played until now, so the latency is there

    std::atomic<bool> decodingAhead; // a worker thread is decoding ahead or was asked to

private:
    ninjam::AudioCodec codec;
    QScopedPointer<AudioDecoder> decoder;
    QMutex decoderMutex; // the decoder is used in the audio thread and in the worker threads

    audio::SamplesBuffer decodedBuffer;
    uint decodedPosition; // the next sample to play in decodedBuffer
    QMutex decodedBufferMutex;

    bool playing; // voice chat waits for some decoded audio before playing
    uint samplesToStartPlaying;

    std::atomic<bool> stopped;

    static const double DECODE_AHEAD_SECONDS;
    static const double VOICE_CHAT_BUFFER_SECONDS;

    inline uint availableSamples() const { return decodedBuffer.getFrameLenght() - decodedPosition; }
    uint samplesToDecodeAhead() const;
    static AudioDecoder *createDecoder(ninjam::AudioCodec codec);
};

// Opus and FLAC decode a whole Ogg page (about half a second) or a FLAC frame at a time, which is
// too slow for the audio thread with many channels. These intervals are decoded some seconds ahead in
// worker threads: when they arrive and in every beat (NinjamController). The audio thread decodes
// only what was not decoded ahead.
const double NinjamTrackNode::IntervalDecoder::DECODE_AHEAD_SECONDS = 3.0;

// Opus and FLAC voice chat intervals wait for one second of audio before playing (Vorbis
// waits for 8 KB), an Ogg Opus page holds about a second of audio at the voice chat bitrate.
const double NinjamTrackNode::IntervalDecoder::VOICE_CHAT_BUFFER_SECONDS = 1.0;

NinjamTrackNode::IntervalDecoder::IntervalDecoder(ninjam::AudioCodec codec, bool isVoiceChat) :
    decodingAhead(false),
    codec(codec),
    decoder(createDecoder(codec)),
    decodedBuffer(2),
    decodedPosition(0),
    playing(true),
    samplesToStartPlaying(0),
    stopped(false)
{
    if (isVoiceChat && codec != ninjam::AudioCodec::Vorbis) {
        playing = false;
        samplesToStartPlaying = 48000 * VOICE_CHAT_BUFFER_SECONDS;
    }
}

AudioDecoder *NinjamTrackNode::IntervalDecoder::createDecoder(ninjam::AudioCodec codec)
{
    switch (codec) {
    case ninjam::AudioCodec::Opus:
        return new opus::Decoder();
    case ninjam::AudioCodec::Flac:
        return new flac::Decoder();
    case ninjam::AudioCodec::Vorbis:
        break;
    }

    return new vorbis::Decoder();
}

void NinjamTrackNode::IntervalDecoder::addEncodedData(const QByteArray &encodedData, bool isLastPart)
{
    // this funcion is called from GUI thread

    QMutexLocker locker(&decoderMutex);
    decoder->addInputData(encodedData);

    if (isLastPart)
        decoder->setInputComplete();
}

uint NinjamTrackNode::IntervalDecoder::samplesToDecodeAhead() const
{
    if (codec == ninjam::AudioCodec::Vorbis)
        return 256; // just the first samples, to avoid slow down the audio thread in interval start (first beat)

    return decoder->getSampleRate() * DECODE_AHEAD_SECONDS;
}

bool NinjamTrackNode::IntervalDecoder::needsDecodingAhead()
{
    // Vorbis is decoded in the audio thread, as needed
    if (codec == ninjam::AudioCodec::Vorbis || stopped || decodingAhead)
        return false;

    QMutexLocker locker(&decodedBufferMutex);
    return availableSamples() < samplesToDecodeAhead() / 2;
}

void NinjamTrackNode::IntervalDecoder::decodeAhead()
{
    {
        QMutexLocker locker(&decoderMutex);
        while (!stopped) {
            {
                QMutexLocker bufferLocker(&decodedBufferMutex);
                if (availableSamples() >= samplesToDecodeAhead())
                    break;
            }

            const auto &decodedSamples = decoder->decode(4096); // the audio thread is not waiting while decoding

            if (decodedSamples.isEmpty())
                break; // waiting for more data or finished

            QMutexLocker bufferLocker(&decodedBufferMutex);
            decodedBuffer.append(decodedSamples);
        }
    }

    decodingAhead = false;
}

void NinjamTrackNode::IntervalDecoder::stopDecoding()
{
    stopped = true; // this funcion is called from GUI thread

    QMutexLocker locker(&decodedBufferMutex);
    decodedBuffer.setFrameLenght(0);
    decodedPosition = 0;
}

bool NinjamTrackNode::IntervalDecoder::isFullyDecoded()
{
    if (stopped)
        return true;

    QMutexLocker locker(&decodedBufferMutex);
    return availableSamples() == 0 && decoder->isFinished();
}

// the samples are written after the first 'outOffset' samples in outBuffer
quint32 NinjamTrackNode::IntervalDecoder::getDecodedSamples(audio::SamplesBuffer &outBuffer, uint samplesToDecode, uint outOffset)
{
    QMutexLocker locker(&decodedBufferMutex);

    if (stopped) {
        outBuffer.setFrameLenght(outOffset);
        return 0;
    }

    // need decode more samples to fill outBuffer? Not waiting while a worker thread is decoding ahead.
    if (availableSamples() < samplesToDecode && decoderMutex.tryLock()) {
        while (availableSamples() < samplesToDecode) {
            const auto &decodedSamples = decoder->decode(samplesToDecode - availableSamples());
            if (decodedSamples.isEmpty())
                break; // no more samples to decode

            decodedBuffer.append(decodedSamples);
        }

        decoderMutex.unlock();
    }

    if (!playing) {
        playing = availableSamples() >= samplesToStartPlaying || decoder->isFinished();
        if (!playing) {
            outBuffer.setFrameLenght(outOffset); // silence while buffering
            return 0;
        }
    }

    quint32 totalSamples = qMin(samplesToDecode, availableSamples());
    outBuffer.setFrameLenght(outOffset + totalSamples);
    outBuffer.set(decodedBuffer, decodedPosition, totalSamples, outOffset);
    decodedPosition += totalSamples;

    if (decodedPosition >= decodedBuffer.getFrameLenght() / 2) { // discard the played samples
        decodedBuffer.discardFirstSamples(decodedPosition);
        decodedPosition = 0;
    }

    return totalSamples;
}

//-------------------------------------------------------------

NinjamTrackNode::NinjamTrackNode(int ID) :
    ID(ID),
    lowCut(new NinjamTrackNode::LowCutFilter(44100))
    //processingLastPartOfInterval(false),
{

}

bool NinjamTrackNode::isStereo() const
{
    if (currentDecoder)
        return currentDecoder->isStereo();

    return true;
}

void NinjamTrackNode::stopDecoding()
{
    discardDownloadedIntervals();

    if (currentDecoder != nullptr)
        currentDecoder->stopDecoding();
}

NinjamTrackNode::LowCutState NinjamTrackNode::setLowCutToNextState()
{
    LowCutState newState = LowCutState::Off;

    switch (lowCut->getState() ) {

    case LowCutState::Off:
        newState = LowCutState::Normal;
        break;
    case LowCutState::Normal:
        newState = LowCutState::Drastic;
        break;
    case LowCutState::Drastic:
        newState = LowCutState::Off;
        break;
    }

    lowCut->setState(newState);

    return newState;
}

NinjamTrackNode::LowCutState NinjamTrackNode::getLowCutState() const
{
    return lowCut->getState();
}

void NinjamTrackNode::setLowCutState(LowCutState newState)
{
    lowCut->setState(newState);
}

int NinjamTrackNode::getSampleRate() const
{
    if (currentDecoder)
        return currentDecoder->getSampleRate();
    return 44100;
}

NinjamTrackNode::~NinjamTrackNode()
{
    //qDebug() << "Deastrutor NinjamTrackNode";

    decodersMutex.lock();
    for (const auto &decoder : decoders)
        decoder->stopDecoding();

    decoders.clear();
    currentDecoder.reset();
    decodersMutex.unlock();
}

void NinjamTrackNode::discardDownloadedIntervals()
{
    QMutexLocker locker(&decodersMutex);

    for (const auto &decoder : decoders)
        decoder->stopDecoding(); // the worker threads decoding ahead can stop

    decoders.clear();

    currentDecoder.reset();

    //qDebug() << "intervals discarded";
}

bool NinjamTrackNode::isPlaying()
{
    QMutexLocker locker(&decodersMutex);

    return currentDecoder != nullptr || mode == VoiceChat; // voice chat is always playing
}

void NinjamTrackNode::consumePendingEvents()
{
    TrackNodeCommand *command = nullptr;
    while (pendingCommands.try_dequeue(command)) {
        command->execute();
        delete command;
    }
}

bool NinjamTrackNode::startNewInterval()
{
    //qDebug() << "--------START INTERVAL------------";

    consumePendingEvents();

    if (mode == Intervalic) {
        decodersMutex.lock();
        if (currentDecoder) {
            currentDecoder->stopDecoding(); //discard the previous interval decoder
            currentDecoder.reset();
        }
        if (!decoders.isEmpty())
            currentDecoder = decoders.takeFirst(); //using the next buffered decoder (next interval)

        decodersMutex.unlock();
    }

    return isPlaying();
}

// this function is used only for voice chat mode. The parameter is not a full interval, it's just a chunk of data.
void NinjamTrackNode::addEncodedChunk(const QByteArray &chunkBytes, bool isFirstPart, bool isLastPart, ninjam::AudioCodec codec)
{
    //qDebug() << "   Chunk received " << chunkBytes.left(4) << "\tFirst:" << isFirstPart << " Last:" << isLastPart << " Bytes received:" << chunkBytes.size();

    if(mode != VoiceChat)
        return;


    QMutexLocker locker(&decodersMutex);

    if (isFirstPart) {
        if (!decoders.isEmpty())
            decoders.last()->addEncodedData(QByteArray(), true); // the previous interval ended without its last part

        decoders.push_back(QSharedPointer<IntervalDecoder>::create(codec, true));
    }
    else if (decoders.isEmpty()) {
        // if decoders are empty and the chunk is not the first part we are receinving partial data of the previous interval, we must wait until receive a new interval
        //qDebug() << "Returning, not the first part of an interval";
        return;
    }

    const auto &decoder = decoders.last();
    decoder->addEncodedData(chunkBytes, isLastPart);

    if (decoder->needsDecodingAhead())
        startDecodingAhead(decoder);
}

 // this function is used only for Intervalic mode. The parameter is a full interval data
void NinjamTrackNode::addEncodedInterval(const QByteArray &fullIntervalBytes, ninjam::AudioCodec codec)
{
    //qDebug() << "Full Interval received " << fullIntervalBytes.left(4);

    if (mode != Intervalic)
        return;

    auto newIntervalDecoder = QSharedPointer<IntervalDecoder>::create(codec, false);
    newIntervalDecoder->addEncodedData(fullIntervalBytes, true);

    decodersMutex.lock();

    decoders.append(newIntervalDecoder);

    decodersMutex.unlock();

    // decoding the first samples (Vorbis) or seconds (Opus and FLAC) in a separated thread to avoid slow down the audio thread
    startDecodingAhead(newIntervalDecoder);
}

void NinjamTrackNode::decodeAhead()
{
    QMutexLocker locker(&decodersMutex);

    if (currentDecoder && currentDecoder->needsDecodingAhead())
        startDecodingAhead(currentDecoder);

    for (const auto &decoder : decoders) {
        if (decoder != currentDecoder && decoder->needsDecodingAhead())
            startDecodingAhead(decoder);
    }
}

void NinjamTrackNode::startDecodingAhead(const QSharedPointer<IntervalDecoder> &decoder)
{
    decoder->decodingAhead = true;

    // the worker holds a reference, the decoder can be discarded while it's decoding
    QThreadPool::globalInstance()->start([decoder]() {
        decoder->decodeAhead();
    });
}

// ++++++++++++++

void NinjamTrackNode::setChannelMode(ChannelMode newMode)
{
    if (newMode != this->mode) {
        this->mode = newMode;
    }
}

// ++++++++++++++++++++++++++++++++++++++

void NinjamTrackNode::schefuleSetChannelMode(ChannelMode mode)
{
    this->mode = NinjamTrackNode::Changing; // nothing is played when is 'changing'

    pendingCommands.enqueue(new ChangeChannelModeCommand(this, mode));

    discardDownloadedIntervals();
}

int NinjamTrackNode::getFramesToProcess(int targetSampleRate, int outFrameLenght)
{
    return needResamplingFor(targetSampleRate) ? getInputResamplingLength(
        getSampleRate(), targetSampleRate, outFrameLenght) : outFrameLenght;
}

void NinjamTrackNode::processReplacing(const audio::SamplesBuffer &in, audio::SamplesBuffer &out,
                                       int sampleRate, std::vector<midi::MidiMessage> &midiBuffer)
{
    if (!isPlaying())
        return;

    bool resampling = false;

    { // mutex scope
        QMutexLocker locker(&decodersMutex);

        if (!currentDecoder) {
            if (mode == VoiceChat && !decoders.isEmpty()) {
                currentDecoder = decoders.first();
                //qDebug() << "USING FIRST DECODER";
            }
            else {
                //qDebug() << "Current decoder is null, not playing!";
                return;
            }
        }

        if (!currentDecoder || !currentDecoder->isValid()) {
            if (currentDecoder && !currentDecoder->isValid()){
                //qDebug() << "Current decoder is not valid, returning!";
                currentDecoder.reset(); // the current decoder is corrupted, setting to nullptr to force a new decoder usage
                decoders.clear();
                internalInputBuffer.zero();
            }
            return;
        }

        auto framesToProcess = getFramesToProcess(sampleRate, out.getFrameLenght());
        internalInputBuffer.setFrameLenght(framesToProcess);

        auto samplesDecoded = currentDecoder->getDecodedSamples(internalInputBuffer, framesToProcess);

        resampling = needResamplingFor(sampleRate); // the samples in internalInputBuffer are in the current decoder sample rate

        if (mode == VoiceChat) { // in voice chat we will not wait until startInterval to use the next available downloaded decoder
            if (currentDecoder->isFullyDecoded()) {
                //qDebug() << "current decoder consumed, using the next decoder";
                auto finishedDecoderSampleRate = currentDecoder->getSampleRate();
                currentDecoder.reset();
                if (!decoders.isEmpty())
                    decoders.removeFirst();

                if (!decoders.isEmpty()) {
                    auto nextDecoder = decoders.first();
                    nextDecoder->skipBuffering(); // the next interval plays right after this one

                    // the rest of this block comes from the next interval, so there is no gap between them
                    if (samplesDecoded < framesToProcess && nextDecoder->getSampleRate() == finishedDecoderSampleRate) {
                        currentDecoder = nextDecoder;
                        currentDecoder->getDecodedSamples(internalInputBuffer, framesToProcess - samplesDecoded, samplesDecoded);
                    }
                }
            }
        }
    }

    if (!internalInputBuffer.isEmpty()) {
        if (resampling) {
            const auto &resampledBuffer = resampler.resample(internalInputBuffer, out.getFrameLenght());
            internalInputBuffer.setFrameLenght(resampledBuffer.getFrameLenght());
            internalInputBuffer.set(resampledBuffer);
        }

        lowCut->process(internalInputBuffer);

        audio::AudioNode::processReplacing(in, out, sampleRate, midiBuffer); // process internal buffer pan, gain, etc
    }
}

bool NinjamTrackNode::needResamplingFor(int targetSampleRate) const
{
    if (currentDecoder)
        return currentDecoder->getSampleRate() != targetSampleRate;

    return false;
}
