#ifndef _NINJAM_H_
#define _NINJAM_H_

#include <QString>
#include <QDataStream>

namespace ninjam {

class NetworkUsageMeasurer // used in network statistics
{
public:
    NetworkUsageMeasurer();
    long getTransferRate() const;
    void addTransferedBytes(qint64 totalBytesTransfered);

private:
    long totalBytesTransfered;
    qint64 lastMeasureTimeStamp;
    long transferRate;
};

enum class MessageType : quint8
{
    AuthChallenge = 0x00,               // received after connect in server
    ClientAuthUser = 0x80,              // sended to server after receive AUTH_CHALLENGE
    AuthReply = 0x01,                   // received after send to auth challenge
    ClientSetChannel = 0x82,            // sended to server after AuthReply describing channels
    ServerConfigChangeNotify = 0x02,    // received when BPI or BPM change
    UserInfoChangeNorify = 0x03,        // received when user add/remove channels or rename a channel
    DownloadIntervalBegin = 0x04, // received when server is notifiyng about the start of a new audio interval stream
    DownloadIntervalWrite = 0x05, // received for every audio interval chunk. We receive a lot of these messages while jamming.
    ServerCodecMask = 0x06,       // received when the room's audio codec changes (only by clients that can decode Opus or FLAC)
    UploadIntervalBegin = 0x83,//
    UploadIntervalWrite = 0x84, //
    ClientSetUserMask = 0x81,
    KeepAlive = 0xfd,                   // server requesting a keepalive. If Jamtaba not respond the server will disconnect.
    ChatMessage = 0xc0,                 // received when users are sending chat messages
    Invalid = 0xff
};

class MessageHeader
{
public:
    MessageHeader(); // invalid/incomplete message header

    static MessageHeader from(QIODevice *device);

    inline bool isValid() const
    {
        return valid;
    }

    inline MessageType getMessageType() const
    {
        return messageType;
    }

    inline quint32 getPayload() const
    {
        return payload;
    }

private:
    MessageHeader(quint8 type, quint32 payload);


    MessageType messageType = MessageType::Invalid;
    quint32 payload = 0;
    bool valid = false;
};

// ++++++++++++++++++++++++++++++++++++++++++

// The audio codecs of the intervals. Servers that know about Opus and FLAC tell the
// clients which one the room uses (MessageType::ServerCodecMask), the others use Vorbis.
enum class AudioCodec : quint8
{
    Vorbis, // 'OGGv', Ogg Vorbis
    Opus,   // 'OPUS', Ogg Opus, one Ogg stream per interval
    Flac    // 'FLAC', native FLAC, one stream per interval
};

// client capabilities bits (sent when authenticating), also used in the server codec mask
const quint32 ClientCapabilityLicenceAgreement = 0x01;
const quint32 ClientCapabilityOpus = 0x100;
const quint32 ClientCapabilityFlac = 0x200;

QByteArray codecFourCC(AudioCodec codec);
bool codecFromFourCC(const QByteArray &fourCC, AudioCodec &codec); // false for other intervals (e.g. JamTaba video)
AudioCodec codecFromMask(quint32 codecMask); // the codec to upload when the server sends this mask
QString codecName(AudioCodec codec); // the name used in the !codec chat command

void serializeString(const QString &string, QDataStream &stream);
void serializeByteArray(const QByteArray &array, QDataStream &stream);

QString extractString(QDataStream &stream);         // ninjam strings are NUL(\0) terminated
QString extractString(QDataStream &stream, quint32 size);

} // namespace

#endif
