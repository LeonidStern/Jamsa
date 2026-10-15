#include "Ninjam.h"

#include <QDateTime>

namespace ninjam {

NetworkUsageMeasurer::NetworkUsageMeasurer() :
    totalBytesTransfered(0),
    lastMeasureTimeStamp(0),
    transferRate(0)
{

}

void NetworkUsageMeasurer::addTransferedBytes(qint64 bytesTransfered)
{
    totalBytesTransfered += bytesTransfered;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (!lastMeasureTimeStamp)
        lastMeasureTimeStamp = now;

    const qint64 ellapsedTime = now - lastMeasureTimeStamp;

    if (ellapsedTime >= 1000) {
        transferRate = totalBytesTransfered / (ellapsedTime / 1000.0);
        totalBytesTransfered = 0;
        lastMeasureTimeStamp = now;
    }
}

long NetworkUsageMeasurer::getTransferRate() const
{
    return transferRate;
}

// ---------------------------------------------------------------

MessageHeader::MessageHeader(quint8 type, quint32 payload) :
    messageType(static_cast<MessageType>(type)),
    payload(payload),
    valid(true)
{

}

MessageHeader::MessageHeader() // invalid header
{

}

MessageHeader MessageHeader::from(QIODevice *device)
{
    QDataStream stream(device);
    stream.setByteOrder(QDataStream::LittleEndian);

    quint8 type;
    quint32 payload;

    stream >> type >> payload;

    return MessageHeader(type, payload);
}

void serializeString(const QString &string, QDataStream &stream)
{
    QByteArray dataArray = string.toUtf8();
    stream.writeRawData(dataArray.data(), dataArray.size());

    stream << quint8('\0'); // NUL TERMINATED
}

void serializeByteArray(const QByteArray &array, QDataStream &stream)
{
    for (int i = 0; i < array.size(); ++i) {
        stream << quint8(array[i]);
    }
}

QString extractString(QDataStream &stream)
{
    quint8 byte;
    QByteArray byteArray;
    while (!stream.atEnd()) {
        stream >> byte;
        if (byte == '\0')
            break;
        byteArray.append(byte);
    }
    return QString::fromUtf8(byteArray.data(), byteArray.size());
}

QString extractString(QDataStream &stream, quint32 size)
{
    QByteArray bytes = stream.device()->read(size);

    // Qt 6 keeps the NUL terminator in the QString (Qt 5 stopped at it)
    int end = bytes.indexOf('\0');
    if (end >= 0)
        bytes.truncate(end);

    return QString::fromUtf8(bytes);
}

// ---------------------------------------------------------------

QByteArray codecFourCC(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::Opus:
        return QByteArrayLiteral("OPUS");
    case AudioCodec::Flac:
        return QByteArrayLiteral("FLAC");
    case AudioCodec::Vorbis:
        break;
    }

    return QByteArrayLiteral("OGGv");
}

bool codecFromFourCC(const QByteArray &fourCC, AudioCodec &codec)
{
    for (auto c : {AudioCodec::Vorbis, AudioCodec::Opus, AudioCodec::Flac}) {
        if (fourCC == codecFourCC(c)) {
            codec = c;
            return true;
        }
    }

    return false;
}

AudioCodec codecFromMask(quint32 codecMask)
{
    if (codecMask & ClientCapabilityFlac)
        return AudioCodec::Flac;

    if (codecMask & ClientCapabilityOpus)
        return AudioCodec::Opus;

    return AudioCodec::Vorbis;
}

QString codecName(AudioCodec codec)
{
    switch (codec) {
    case AudioCodec::Opus:
        return QStringLiteral("opus");
    case AudioCodec::Flac:
        return QStringLiteral("flac");
    case AudioCodec::Vorbis:
        break;
    }

    return QStringLiteral("vorbis");
}

} // namespace


