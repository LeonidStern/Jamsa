#ifndef NINJAM_PRIVATE_SERVER_H
#define NINJAM_PRIVATE_SERVER_H

#include <QObject>
#include <QStringList>

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

namespace ninjam {

/**
    The NINJAM server hosted from the "Private Server" window: the server from this
    repository (ninjam/server), built into JamTaba and run on a background thread.

    It asks the router to forward its port (UPnP or NAT-PMP), lets anyone join without
    an account, and lets the room switch to Opus or FLAC. Only one can run at a time.
 */
class PrivateServer : public QObject
{
    Q_OBJECT

public:
    explicit PrivateServer(QObject *parent = nullptr);
    ~PrivateServer() override;

    // Asynchronous: serverStarted() or errorStartingServer() is emitted later.
    // If the port is taken, the next few ports are tried.
    void start(quint16 port);

    // Stops a running server; serverStopped() is emitted when it is stopped.
    void shutdown();

    bool isStarted() const;

    quint16 getPort() const; // 0 when the server is not running

    // "ip:port" that people outside the local network can connect to, once the router
    // forwards the port; empty until then
    QString getPublicAddress() const;

    QStringList getConnectedUsersNames() const;

    // bytes per second, 0 when unknown
    quint64 getDownloadTransferRate() const;
    quint64 getUploadTransferRate() const;

signals:
    void serverStarted();
    void errorStartingServer(const QString &errorMessage);
    void serverStopped();
    void incommingConnection(const QString &incommingIP);
    void userEntered(const QString &userName);
    void userLeave(const QString &userName);

    // what the host should know: whether the router forwards the port, and errors
    void serverMessage(const QString &text);
    void publicAddressChanged(const QString &address);
    void portMappingFailed(); // people outside the local network can't connect; serverMessage() says why

private:
    static void onServerLog(void *ctx, const char *text); // called on the server's threads
    void handleServerLog(const QString &line);
    void updateConnectedUsers();
    QString writeConfig(quint16 port) const;

    bool started;
    quint16 port;
    QString publicAddress;
    QStringList connectedUsers;
    QTimer *usersTimer;
};

} // namespace

#endif // NINJAM_PRIVATE_SERVER_H
