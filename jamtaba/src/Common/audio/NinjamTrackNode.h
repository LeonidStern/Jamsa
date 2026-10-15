#ifndef NINJAMTRACKNODE_H
#define NINJAMTRACKNODE_H

#include "core/AudioNode.h"
#include <QByteArray>
#include <QSharedPointer>
#include "SamplesBufferResampler.h"
#include "readerwriterqueue.h"
#include "ninjam/Ninjam.h"

namespace audio {
class SamplesBuffer;
class StreamBuffer;
}

class NinjamTrackNode : public audio::AudioNode
{

public:

    enum LowCutState
    {
        Off, Normal, Drastic
    };

    enum ChannelMode {
        Intervalic,
        VoiceChat,
        Changing // used when waiting for the next interval do change the mode. Nothing is played in this 'transition state mode'
    };

    explicit NinjamTrackNode(int ID);
    virtual ~NinjamTrackNode();
    void addEncodedInterval(const QByteArray &fullIntervalBytes, ninjam::AudioCodec codec);
    void addEncodedChunk(const QByteArray &chunkBytes, bool isFirstPart, bool isLastPart, ninjam::AudioCodec codec);
    void decodeAhead(); // decode the next seconds of Opus and FLAC intervals in a worker thread
    void processReplacing(const audio::SamplesBuffer &in, audio::SamplesBuffer &out, int sampleRate,
                          std::vector<midi::MidiMessage> &midiBuffer) override;

    void setLowCutState(LowCutState newState);
    LowCutState setLowCutToNextState();
    LowCutState getLowCutState() const;

    bool startNewInterval();
    int getID() const;

    int getSampleRate() const;

    bool isPlaying();

    bool isStereo() const;

    bool isIntervalic() const { return mode == Intervalic; }

    bool isVoiceChat() const { return mode == VoiceChat; }

    void schefuleSetChannelMode(ChannelMode mode);

    // Discard all downloaded (but not played yet) intervals
    void discardDownloadedIntervals();

    void stopDecoding();

    //void setProcessingLastPartOfInterval(bool status);

protected:

    class TrackNodeCommand {
    protected:
        TrackNodeCommand(NinjamTrackNode *node);
        NinjamTrackNode *trackNode;

    public:
        virtual ~TrackNodeCommand() {}
        virtual void execute() = 0;
    };

    class ChangeChannelModeCommand;

    void setChannelMode(ChannelMode newMode);

private:
    int ID;
    SamplesBufferResampler resampler;

    class LowCutFilter;
    QScopedPointer<LowCutFilter> lowCut;
    const static double LOW_CUT_NORMAL_FREQUENCY;
    const static double LOW_CUT_DRASTIC_FREQUENCY;

    bool needResamplingFor(int targetSampleRate) const;

    int getFramesToProcess(int targetSampleRate, int outFrameLenght);

    //bool processingLastPartOfInterval;

    class IntervalDecoder;

    // shared with the worker threads decoding ahead, a decoder can be discarded while it's decoding
    QList<QSharedPointer<IntervalDecoder>> decoders;
    QSharedPointer<IntervalDecoder> currentDecoder;
    QMutex decodersMutex;

    static void startDecodingAhead(const QSharedPointer<IntervalDecoder> &decoder);

    ChannelMode mode = Intervalic;

    moodycamel::ReaderWriterQueue<TrackNodeCommand *> pendingCommands;

    void consumePendingEvents();

};


inline int NinjamTrackNode::getID() const
{
    return ID;
}

#endif // NINJAMTRACKNODE_H
