#include "PrivateServerWindow.h"
#include "ui_PrivateServerWindow.h"
#include "ninjam/PrivateServer.h"

#include <QDateTime>
#include <QMessageBox>

using ninjam::PrivateServer;

const quint16 PrivateServerWindow::PREFERRED_PORT = 2049;

PrivateServerWindow::PrivateServerWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::PrivateServerWindow),
    networkUsageTimerID(0)
{
    ui->setupUi(this);
    
#ifdef Q_OS_MAC
    setWindowFlags(windowFlags() | Qt::Popup);
#endif

    connect(ui->pushButtonStart, &QPushButton::clicked, this, &PrivateServerWindow::startServer);
    connect(ui->pushButtonStop, &QPushButton::clicked, this, &PrivateServerWindow::stopServer);

    ui->pushButtonStart->setEnabled(true);
    ui->pushButtonStop->setEnabled(false);

    setServerDetailsVisibility(false);

    ui->labelIPValue->setText(QString());
    ui->labelPortValue->setText(QString());
}

bool PrivateServerWindow::serverIsRunning() const
{
    if (!server)
        return false;

    return server->isStarted();
}

quint16 PrivateServerWindow::getServerPort() const
{
    if (!serverIsRunning())
        return 0;

    return server->getPort();
}

QString PrivateServerWindow::getServerExternalIP() const
{
    // the external IP was discovered with UPnP, which was removed
    return QString();
}

PrivateServerWindow::~PrivateServerWindow()
{

    server.reset(nullptr);

    delete ui;
}

void PrivateServerWindow::setupServer()
{
    if (!server)
        return;

    connect(server.data(), &PrivateServer::serverStarted, this, &PrivateServerWindow::serverStarted);
    connect(server.data(), &PrivateServer::serverStopped, this, &PrivateServerWindow::serverStopped);

    connect(server.data(), &PrivateServer::errorStartingServer, [=](const QString &errorMessage){
        appendTextInLog(errorMessage);
    });

    connect(server.data(), &PrivateServer::incommingConnection, [=](const QString &ip){
        appendTextInLog(tr("Incomming connection from %1").arg(ip));
    });

    connect(server.data(), &PrivateServer::userEntered, [=](const QString &userName){
        appendTextInLog(tr("%1 entered in the server").arg(userName));

        updateUserList();
    });

    connect(server.data(), &PrivateServer::userLeave, [=](const QString &userName){
        appendTextInLog(tr("%1 left the server").arg(userName));

        updateUserList();
    });


}

void PrivateServerWindow::appendTextInLog(const QString &text)
{
    QString now = QDateTime::currentDateTime().toUTC().toString("hh:mm:ss");
    ui->textBrowser->append(QString("[%1] %2").arg(now).arg(text));
}

void PrivateServerWindow::updateUserList()
{
    if (!server)
        return;

    ui->listWidget->clear();
    for (auto userName : server->getConnectedUsersNames()) {
        ui->listWidget->addItem(userName);
    }
}

void PrivateServerWindow::serverStarted()
{
    ui->textBrowser->append(QString());
    appendTextInLog(tr("Server started"));

    ui->pushButtonStart->setEnabled(false);
    ui->pushButtonStop->setEnabled(true);

    ui->listWidget->clear();

    if (networkUsageTimerID)
        killTimer(networkUsageTimerID);

    networkUsageTimerID = startTimer(3000);

    // the external IP was discovered with UPnP, which was removed, so only the port is shown
    ui->labelPortValue->setText(QString::number(server->getPort()));

    setServerDetailsVisibility(true);
    ui->labelIP->setVisible(false);
    ui->labelIPValue->setVisible(false);

    QString title = tr("Your Private Server");
    QString text = tr("Connect with private server ...");
    auto buttons = QMessageBox::StandardButtons(QMessageBox::Yes|QMessageBox::No);
    int result = QMessageBox::question(this, title, text, buttons);
    if (result == QMessageBox::Yes) {
        emit userConnectingInPrivateServer(QStringLiteral("127.0.0.1"), server->getPort());
    }
}

void PrivateServerWindow::serverStopped()
{
    appendTextInLog(tr("Server stopped!"));

    ui->pushButtonStart->setEnabled(true);
    ui->pushButtonStop->setEnabled(false);

    ui->labelIPValue->setText(QString());
    ui->labelPortValue->setText(QString());

    setServerDetailsVisibility(false);

    ui->listWidget->clear();

    if (networkUsageTimerID)
        killTimer(networkUsageTimerID);
}

void PrivateServerWindow::setServerDetailsVisibility(bool visible)
{
    ui->labelIP->setVisible(visible);
    ui->labelPort->setVisible(visible);
    ui->labelIPValue->setVisible(visible);
    ui->labelPortValue->setVisible(visible);

    ui->labelDownload->setVisible(visible);
    ui->labelDownloadValue->setVisible(visible);
    ui->labelUpload->setVisible(visible);
    ui->labelUploadValue->setVisible(visible);
}

void PrivateServerWindow::startServer()
{
    if (!server) {
        server.reset(new PrivateServer());
        setupServer();
    }

    if (!server->isStarted()) {
        quint16 port = PREFERRED_PORT;
        server->start(port);
    }
}

void PrivateServerWindow::stopServer()
{
    if (!server)
        return;

    if (server->isStarted())
        server->shutdown();
}

void PrivateServerWindow::timerEvent(QTimerEvent *)
{
    if (!server)
        return;

    quint64 download = server->getDownloadTransferRate() / 1024 * 8; // Kbps
    quint64 upload = server->getUploadTransferRate() / 1024 * 8;
    ui->labelDownloadValue->setText(QString::number(download));
    ui->labelUploadValue->setText(QString::number(upload));
}

void PrivateServerWindow::changeEvent(QEvent *ev)
{
    if (ev->type() == QEvent::LanguageChange) {

        // translate strings typed directly in PrivateServerWindow.ui file
        ui->retranslateUi(this);
    }

    QDialog::changeEvent(ev);
}
