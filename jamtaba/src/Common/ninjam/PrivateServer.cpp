#include "PrivateServer.h"

#include "Configurator.h"
#include "ninjamsrv_embedded.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QRegularExpression>
#include <QTextStream>
#include <QTimer>

using ninjam::PrivateServer;

namespace {

const int PORTS_TO_TRY = 10; // when the preferred port is taken, the next ones are tried

// Anyone can join without an account, any player can change BPM and BPI (a single
// vote passes) or switch the room to Opus or FLAC (!codec), and the server asks the
// router to forward its port.
const char *CONFIG =
        "# Written by JamTaba each time it starts the private server.\n"
        "Port %1\n"
        "MaxUsers 8\n"
        "MaxChannels 32 32\n"
        "AnonymousUsers yes\n"
        "AnonymousUsersCanChat yes\n"
        "AnonymousMaskIP no\n"
        "DefaultBPM 120\n"
        "DefaultBPI 16\n"
        "SetVotingThreshold 1\n"
        "SetVotingTimeout 60\n"
        "AllowCodecs opus flac\n"
        "PortMapping yes\n";

} // namespace

PrivateServer::PrivateServer(QObject *parent) :
    QObject(parent),
    started(false),
    port(0),
    usersTimer(new QTimer(this))
{
    connect(usersTimer, &QTimer::timeout, this, &PrivateServer::updateConnectedUsers);
}

PrivateServer::~PrivateServer()
{
    // waits for the server thread, so no log callback can reach this object afterwards
    if (started)
        ninjamsrv_embedded_stop();
}

QString PrivateServer::writeConfig(quint16 port) const
{
    QDir dir = Configurator::getInstance()->getBaseDir();
    QString path = dir.absoluteFilePath("private-server.cfg");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return QString();

    QTextStream stream(&file);
    stream << QString(CONFIG).arg(port);
    return path;
}

void PrivateServer::start(quint16 preferredPort)
{
    if (started)
        return;

    QString error;
    int tryPort = preferredPort;
    for (int i = 0; i < PORTS_TO_TRY && !started; ++i, ++tryPort) {
        QString configPath = writeConfig(tryPort);
        if (configPath.isEmpty()) {
            error = tr("Could not write the server settings to %1").arg(Configurator::getInstance()->getBaseDir().absolutePath());
            break;
        }

        int result = ninjamsrv_embedded_start(QDir::toNativeSeparators(configPath).toLocal8Bit().constData(), &PrivateServer::onServerLog, this);
        if (result == NINJAMSRV_OK) {
            started = true;
            port = tryPort;
        }
        else if (result == NINJAMSRV_LISTEN_ERROR) {
            error = tr("Ports %1 to %2 are in use by other programs.").arg(preferredPort).arg(tryPort);
        }
        else {
            error = result == NINJAMSRV_ALREADY_RUNNING ? tr("The server is already running.")
                                                        : tr("The server could not read its settings.");
            break;
        }
    }

    QMetaObject::invokeMethod(this, [this, error]() {
        if (started) {
            usersTimer->start(1000);
            emit serverStarted();
        }
        else {
            emit errorStartingServer(error);
        }
    }, Qt::QueuedConnection);
}

void PrivateServer::shutdown()
{
    if (!started)
        return;

    usersTimer->stop();
    ninjamsrv_embedded_stop(); // also removes the router's port forward

    started = false;
    port = 0;
    publicAddress.clear();
    connectedUsers.clear();

    emit serverStopped();
}

void PrivateServer::onServerLog(void *ctx, const char *text)
{
    auto server = static_cast<PrivateServer *>(ctx);
    QString line = QString::fromLocal8Bit(text).trimmed();
    QMetaObject::invokeMethod(server, [server, line]() {
        server->handleServerLog(line);
    }, Qt::QueuedConnection);
}

void PrivateServer::handleServerLog(const QString &line)
{
    if (!started)
        return;

    qDebug() << "ninjamsrv:" << line;

    static const QRegularExpression incomming("^Incoming connection from (.+)!$");
    auto match = incomming.match(line);
    if (match.hasMatch()) {
        emit incommingConnection(match.captured(1));
        return;
    }

    if (line.startsWith("[portmap]")) {
        static const QRegularExpression reachable("people can connect to (\\S+)$");
        match = reachable.match(line);
        if (match.hasMatch()) {
            publicAddress = match.captured(1);
            emit publicAddressChanged(publicAddress);
        }
        else if (line.contains("removed the forward") || line.contains("lost the forward")) {
            publicAddress.clear();
            emit publicAddressChanged(publicAddress);
        }

        emit serverMessage(line.mid(QString("[portmap]").size()).trimmed());
    }
    else if (line.startsWith("Error listening")) {
        return; // a port that was taken while starting; start() tries the next one
    }
    else if (line.startsWith("Error", Qt::CaseInsensitive) || line.startsWith("warning", Qt::CaseInsensitive)) {
        emit serverMessage(line);
    }
}

void PrivateServer::updateConnectedUsers()
{
    QByteArray buffer(4096, '\0');
    ninjamsrv_embedded_users(buffer.data(), buffer.size());

    QStringList users = QString::fromUtf8(buffer.constData()).split('\n', Qt::SkipEmptyParts);
    QStringList previous = connectedUsers;
    connectedUsers = users;

    for (const QString &user : users) {
        if (!previous.contains(user))
            emit userEntered(user);
    }
    for (const QString &user : previous) {
        if (!users.contains(user))
            emit userLeave(user);
    }
}

bool PrivateServer::isStarted() const
{
    return started;
}

quint16 PrivateServer::getPort() const
{
    return port;
}

QString PrivateServer::getPublicAddress() const
{
    return publicAddress;
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
