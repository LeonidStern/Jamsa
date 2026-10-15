#ifndef PRIVATESERVERWINDOW_H
#define PRIVATESERVERWINDOW_H

#include <QDialog>
#include <QScopedPointer>

QT_BEGIN_NAMESPACE
namespace Ui {
class PrivateServerWindow;
}
QT_END_NAMESPACE

namespace ninjam {
    class PrivateServer;
}

class PrivateServerWindow : public QDialog
{
    Q_OBJECT

public:
    explicit PrivateServerWindow(QWidget *parent = 0);
    ~PrivateServerWindow();
    bool serverIsRunning() const;
    quint16 getServerPort() const;
    QString getServerExternalIP() const;

signals:
    void userConnectingInPrivateServer(QString server, quint16 port);

protected:
    void changeEvent(QEvent *) override;

private slots:
    void startServer();
    void stopServer();

    void serverStarted();
    void serverStopped();
    void showPublicAddress(const QString &address);

private:
    Ui::PrivateServerWindow *ui;

    void setupServer();
    void setServerDetailsVisibility(bool enabled);

    void updateUserList();

    void appendTextInLog(const QString &text);

    QScopedPointer<ninjam::PrivateServer> server;

    static const quint16 PREFERRED_PORT;
};

#endif // PRIVATESERVERWINDOW_H
