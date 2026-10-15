#ifndef NINJAM_PRIVATE_SERVER_H
#define NINJAM_PRIVATE_SERVER_H

#include <QObject>
#include <QStringList>

namespace ninjam {

/**
    The NINJAM server hosted from the "Private Server" window.

    The API only offers what the window needs (start on a port, stop, running state and
    status/error/user notifications), so it can be backed by an external server process.

    For now this is a stub: start() always fails with a "not available yet" error.
 */
class PrivateServer : public QObject
{
    Q_OBJECT

public:
    explicit PrivateServer(QObject *parent = nullptr);
    ~PrivateServer() override;

    // Asynchronous: serverStarted() or errorStartingServer() is emitted later.
    void start(quint16 port);

    // Stops a running server; serverStopped() is emitted when it is stopped.
    void shutdown();

    bool isStarted() const;

    quint16 getPort() const; // 0 when the server is not running

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

private:
    bool started;
    quint16 port;
    QStringList connectedUsers;
};

} // namespace

#endif // NINJAM_PRIVATE_SERVER_H
