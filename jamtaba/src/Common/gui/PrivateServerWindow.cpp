#include "PrivateServerWindow.h"
#include "ui_PrivateServerWindow.h"
#include "ninjam/PrivateServer.h"

#include <QDateTime>
#include <QMessageBox>

using ninjam::PrivateServer;

const quint16 PrivateServerWindow::PREFERRED_PORT = 2049;

PrivateServerWindow::PrivateServerWindow(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::PrivateServerWindow)
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
    if (!serverIsRunning())
        return QString();

    // "ip:port", known once the router forwards the port
    return server->getPublicAddress().section(':', 0, 0);
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

    connect(server.data(), &PrivateServer::serverMessage, this, &PrivateServerWindow::appendTextInLog);

    connect(server.data(), &PrivateServer::publicAddressChanged, this, &PrivateServerWindow::showPublicAddress);

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

    ui->labelPortValue->setText(QString::number(server->getPort()));

    setServerDetailsVisibility(true);
    showPublicAddress(server->getPublicAddress());

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
}

void PrivateServerWindow::showPublicAddress(const QString &address)
{
    // the IP to give to people outside your network; the router reports it once it forwards the port
    QString ip = address.section(':', 0, 0);
    ui->labelIPValue->setText(ip.isEmpty() ? tr("waiting for the router") : ip);
    ui->labelIPValue->setTextInteractionFlags(Qt::TextSelectableByMouse);
}

void PrivateServerWindow::setServerDetailsVisibility(bool visible)
{
    ui->labelIP->setVisible(visible);
    ui->labelPort->setVisible(visible);
    ui->labelIPValue->setVisible(visible);
    ui->labelPortValue->setVisible(visible);

    // the built-in server doesn't measure its traffic
    ui->labelDownload->setVisible(false);
    ui->labelDownloadValue->setVisible(false);
    ui->labelUpload->setVisible(false);
    ui->labelUploadValue->setVisible(false);
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

void PrivateServerWindow::changeEvent(QEvent *ev)
{
    if (ev->type() == QEvent::LanguageChange) {

        // translate strings typed directly in PrivateServerWindow.ui file
        ui->retranslateUi(this);
    }

    QDialog::changeEvent(ev);
}
