#include "ServerMessagesHandler.h"
#include "ServerMessages.h"

using namespace ninjam;
using namespace ninjam::client;

ServerMessagesHandler::ServerMessagesHandler(Service *service) :
    service(service),
    device(nullptr)
{
    //
}

ServerMessagesHandler::~ServerMessagesHandler()
{
    //
}

void ServerMessagesHandler::initialize(QIODevice *device)
{
    Q_ASSERT(device);
    this->device = device;
    currentHeader = MessageHeader();
}

void ServerMessagesHandler::handleAllMessages()
{
    Q_ASSERT(device);
    forever { // consume all messages
        if (!currentHeader.isValid()) {
            if (device->bytesAvailable() < 5) // every ninjam message contains a 5 bytes header
                break;

            currentHeader = MessageHeader::from(device);
        }

        bool successfullyProcessed = executeMessageHandler(currentHeader);
        if (successfullyProcessed)
            currentHeader = MessageHeader(); // invalidate the current message header, a new header will be readed from socket in the next loop iteration
        else
            break; // an incomplete message was founded, break and wait until receive more bytes. The currentHeader will be used in the next iteration.
    }
}

bool ServerMessagesHandler::executeMessageHandler(const MessageHeader &header)
{
    Q_ASSERT(header.isValid());

    switch (header.getMessageType()) {
    case MessageType::AuthChallenge:
        return handleMessage<AuthChallengeMessage>(header.getPayload());
    case MessageType::AuthReply:
        return handleMessage<AuthReplyMessage>(header.getPayload());
    case MessageType::ServerConfigChangeNotify:
        return handleMessage<ConfigChangeNotifyMessage>(header.getPayload());
    case MessageType::UserInfoChangeNorify:
        return handleMessage<UserInfoChangeNotifyMessage>(header.getPayload());
    case MessageType::KeepAlive:
        return handleMessage<ServerKeepAliveMessage>(header.getPayload());
    case MessageType::ChatMessage:
        return handleMessage<ServerToClientChatMessage>(header.getPayload());
    case MessageType::DownloadIntervalBegin:
        return handleMessage<DownloadIntervalBegin>(header.getPayload());
    case MessageType::DownloadIntervalWrite:
        return handleMessage<DownloadIntervalWrite>(header.getPayload());
    case MessageType::ServerCodecMask:
        return handleMessage<ServerCodecMaskMessage>(header.getPayload());
    default:
        break;
    }

    qCDebug(jtNinjamProtocol) << "Skipping the unknown message code" << static_cast<quint8>(header.getMessageType());
    return skipMessage(header.getPayload());
}

bool ServerMessagesHandler::skipMessage(quint32 payload)
{
    if (device->bytesAvailable() < payload)
        return false; // wait for the whole message

    device->skip(payload);
    return true;
}
