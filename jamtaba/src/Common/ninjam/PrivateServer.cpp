#include "PrivateServer.h"

#include <QMetaObject>

using ninjam::PrivateServer;

PrivateServer::PrivateServer(QObject *parent) :
    QObject(parent),
    started(false),
    port(0)
{

}

PrivateServer::~PrivateServer()
{

}

void PrivateServer::start(quint16 port)
{
    Q_UNUSED(port);

    if (started)
        return;

    // TODO: run the ninjamsrv program. Until then hosting a server is not available.
    QMetaObject::invokeMethod(this, [this]() {
        emit errorStartingServer(tr("Hosting a private server is not available yet."));
    }, Qt::QueuedConnection);
}

void PrivateServer::shutdown()
{
    if (!started)
        return;

    started = false;
    port = 0;
    connectedUsers.clear();

    emit serverStopped();
}

bool PrivateServer::isStarted() const
{
    return started;
}

quint16 PrivateServer::getPort() const
{
    return port;
}

QStringList PrivateServer::getConnectedUsersNames() const
{
    return connectedUsers;
}

quint64 PrivateServer::getDownloadTransferRate() const
{
    return 0;
}

quint64 PrivateServer::getUploadTransferRate() const
{
    return 0;
}
