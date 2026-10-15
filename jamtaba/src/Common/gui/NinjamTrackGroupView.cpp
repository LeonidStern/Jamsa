#include "NinjamTrackGroupView.h"
#include "MainController.h"
#include "NinjamController.h"
#include "IconFactory.h"
#include "ninjam/client/Service.h"
#include "MainWindow.h"

#include <QMenu>
#include <QDateTime>
#include <QLayout>
#include <QStackedLayout>

const uint NinjamTrackGroupView::MAX_WIDTH_IN_GRID_LAYOUT = 350;
const uint NinjamTrackGroupView::MAX_HEIGHT_IN_GRID_LAYOUT = 210;

using controller::MainController;
using controller::NinjamController;
using persistence::CacheEntry;

NinjamTrackGroupView::NinjamTrackGroupView(MainController *mainController, long trackID,
                                           const QString &channelName, NinjamTrackNode::ChannelMode channelMode, const QColor &userColor,
                                           const CacheEntry &initialValues) :
    TrackGroupView(nullptr),
    mainController(mainController),
    userIP(initialValues.getUserIP()),
    tracksLayoutEnum(TracksLayout::VerticalLayout)
{

    // change the top panel layout to vertical (original is horizontal)
    topPanelLayout->setDirection(QHBoxLayout::TopToBottom);
    topPanelLayout->setContentsMargins(0, 0, 0, 0);
    topPanelLayout->setSpacing(3);

    userNameLabel = new MarqueeLabel();
    userNameLabel->setObjectName("groupNameField");
    userNameLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    QHBoxLayout *groupNameLayout = new QHBoxLayout();
    groupNameLayout->addWidget(userNameLabel, 1);
    chatBlockIconLabel = new QLabel(this);
    chatBlockIconLabel->setPixmap(QPixmap(":/images/chat_blocked.png"));

    setUserName(initialValues.getUserName());

    auto blocked = mainController->userIsBlockedInChat(getUniqueName());
    chatBlockIconLabel->setVisible(blocked);

    groupNameLayout->addWidget(chatBlockIconLabel);
    topPanelLayout->addLayout(groupNameLayout);
    topPanelLayout->setAlignment(groupNameLayout, Qt::AlignBottom);

    // country flag and label
    countryLabel = new QLabel();
    countryLabel->setObjectName("countryLabel");
    topPanelLayout->setAlignment(countryLabel, Qt::AlignTop);

    countryFlag = new QLabel();

    updateGeoLocation();

    topPanelLayout->addSpacing(0); // fixing Skinny user names #914
    topPanelLayout->addWidget(countryFlag);
    topPanelLayout->addWidget(countryLabel);
    topPanelLayout->setAlignment(countryFlag, Qt::AlignCenter);

    // create the first subchannel by default
    NinjamTrackView *newTrackView = addTrackView(trackID);
    newTrackView->setChannelName(channelName);
    newTrackView->setChannelMode(channelMode);
    newTrackView->setInitialValues(initialValues);

    QString styleSheet = "background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, ";
    styleSheet += "stop: 0 rgba(0, 0, 0, 0), ";
    styleSheet += "stop: 0.35 " + userColor.name() + ", ";
    styleSheet += "stop: 0.65" + userColor.name() + ", ";
    styleSheet += "stop: 1 rgba(0, 0, 0, 0));";
    userNameLabel->setStyleSheet(styleSheet);

    //connect(mainController, &MainController::ipResolved, this, &NinjamTrackGroupView::updateGeoLocation);
    connect(mainController, SIGNAL(ipResolved(QString)), this, SLOT(updateGeoLocation(QString)));

    // reacting to chat block/unblock events
    connect(mainController, &MainController::userBlockedInChat, this, &NinjamTrackGroupView::showChatBlockIcon);
    connect(mainController, &MainController::userUnblockedInChat, this, &NinjamTrackGroupView::hideChatBlockIcon);

    setupVerticalLayout();
}

QColor NinjamTrackGroupView::getTintColor() const
{
    if (trackViews.empty())
        return Qt::black;

    return trackViews.first()->getTintColor();
}

QString NinjamTrackGroupView::getUniqueName() const
{
    QString userName = userNameLabel->text();
    if (!userName.contains("@")) {
        return QString("%1@%2")
                .arg(userName)
                .arg(userIP);
    }

    return userName;
}

void NinjamTrackGroupView::hideChatBlockIcon(const QString &unblockedUserName)
{
    if (unblockedUserName == getUniqueName())
        chatBlockIconLabel->hide();
}

void NinjamTrackGroupView::showChatBlockIcon(const QString &blockedUserName)
{
    if (blockedUserName == getUniqueName())
        chatBlockIconLabel->show();
}

void NinjamTrackGroupView::populateContextMenu(QMenu &contextMenu)
{
    QString userName = userNameLabel->text();

    //bool userIsBlockedInChat = mainController->userIsBlockedInChat(getUniqueName());

    QAction *privateChatAction = contextMenu.addAction(tr("Private chat with %1").arg(userName));
    connect(privateChatAction, &QAction::triggered, this, [=]() {

        emit createPrivateChat(userName, userIP);

    });

    QString localIP;
    auto service = mainController->getNinjamService();
    if (service) {
        localIP = ninjam::client::extractUserIP(service->getConnectedUserName());
    }
    privateChatAction->setEnabled(!userIP.isEmpty() && !localIP.isEmpty()); // admin IPs are empty

    contextMenu.addSeparator();

    auto mainWindow = mainController->getMainWindow();
    if (mainWindow) {
        mainWindow->fillUserContextMenu(contextMenu, getUniqueName(), false);
    }

    TrackGroupView::populateContextMenu(contextMenu);
}

void NinjamTrackGroupView::blockChatMessages()
{
    mainController->blockUserInChat(getUniqueName());
}

void NinjamTrackGroupView::unblockChatMessages()
{
    mainController->unblockUserInChat(getUniqueName());
}

void NinjamTrackGroupView::updateGeoLocation(const QString &ip)
{
    if (ninjam::client::maskIP(ip) != ninjam::client::maskIP(this->userIP))
        return;

    auto location = mainController->getGeoLocation(ip);
    QString countryCode = location.countryCode.toLower();
    QString flagImage = ":/flags/flags/" + countryCode +".png";
    countryFlag->setPixmap(QPixmap(flagImage));
    countryLabel->setText(location.countryName);
}

void NinjamTrackGroupView::updateGeoLocation()
{
    updateGeoLocation(this->userIP);
}

void NinjamTrackGroupView::setEstimatedChunksPerInterval(int estimatedChunks)
{
    for (NinjamTrackView * track : getTracks<NinjamTrackView *>()) {
        track->setEstimatedChunksPerInterval(estimatedChunks);
    }
}

QString NinjamTrackGroupView::getRgbaColorString(const QColor &color, int alpha)
{
    int red = color.red();
    int green = color.green();
    int blue = color.blue();
    return "rgba(" + QString::number(red) + "," + QString::number(green) + "," + QString::number(blue) + "," + QString::number(alpha) + ")";
}

void NinjamTrackGroupView::setTracksLayout(TracksLayout newLayout)
{
    tracksLayoutEnum = newLayout;
    auto tracks = getTracks<NinjamTrackView *>();
    Qt::Orientation orientation = getTracksOrientation();
    for (NinjamTrackView *track : tracks) {
        track->setOrientation(orientation);
    }

    if (newLayout == TracksLayout::HorizontalLayout) {
        setupHorizontalLayout();
        topPanel->setFixedWidth(100); // using fixed width in horizontal layout
    }
    else if (newLayout == TracksLayout::VerticalLayout) {
        setupVerticalLayout();
        topPanel->setMaximumWidth(QWIDGETSIZE_MAX);
        topPanel->setMinimumWidth(1);
    }
    else if(newLayout == TracksLayout::GridLayout) {
        setupGridLayout();
        topPanel->setMaximumWidth(QWIDGETSIZE_MAX);
        topPanel->setMinimumWidth(1);
    }
    else {
        qCritical() << "Invalid layout " << static_cast<quint8>(newLayout);
    }

    topPanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    setProperty("horizontal", orientation == Qt::Horizontal ? true : false);
    refreshStyleSheet();

    updateGeometry();
}

void NinjamTrackGroupView::setupGridLayout()
{
    mainLayout->removeWidget(topPanel);
    mainLayout->removeItem(tracksLayout);

    mainLayout->addWidget(topPanel, 0, 0, mainLayout->rowCount(), 1);
    mainLayout->addLayout(tracksLayout, 0, 1, 1, 1);

    resetMainLayoutStretch();
    mainLayout->setColumnStretch(0, 1);
    mainLayout->setRowStretch(0, 1);

    mainLayout->setSpacing(3);

    tracksLayout->setDirection(QBoxLayout::LeftToRight);

    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Maximum);
}

void NinjamTrackGroupView::setupHorizontalLayout()
{
    mainLayout->removeWidget(topPanel);
    mainLayout->removeItem(tracksLayout);

    mainLayout->addWidget(topPanel,    0, 0, 1, 1);
    mainLayout->addLayout(tracksLayout,      0, 1, 1, 1);

    mainLayout->setSpacing(0);

    resetMainLayoutStretch();
    mainLayout->setColumnStretch(1, 1); // tracks are strechted

    tracksLayout->setDirection(QBoxLayout::TopToBottom);
    topPanelLayout->setDirection(QBoxLayout::TopToBottom);

    topPanel->setVisible(true);

    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Maximum);
}

void NinjamTrackGroupView::setupVerticalLayout()
{
    mainLayout->removeWidget(topPanel);
    mainLayout->removeItem(tracksLayout);

    mainLayout->addWidget(topPanel, 0, 0, 1, 1);
    mainLayout->addLayout(tracksLayout, 1, 0, 1, 1);

    mainLayout->setSpacing(0);

    resetMainLayoutStretch();
    mainLayout->setColumnStretch(0, 1);
    mainLayout->setRowStretch(1, 1); // tracks are strechted

    topPanel->setVisible(true);

    tracksLayout->setDirection(QBoxLayout::LeftToRight);
    topPanelLayout->setDirection(QBoxLayout::TopToBottom);

    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
}

void NinjamTrackGroupView::resetMainLayoutStretch()
{
    int rows = mainLayout->rowCount();
    for (int r = 0; r < rows; ++r) {
        mainLayout->setRowStretch(r, 0);
    }

    int collumns = mainLayout->columnCount();
    for (int c = 0; c < collumns; ++c) {
        mainLayout->setColumnStretch(c, 0);
    }
}

NinjamTrackView *NinjamTrackGroupView::createTrackView(long trackID)
{
    return new NinjamTrackView(mainController, trackID);
}

void NinjamTrackGroupView::setUserName(const QString &userName)
{
    userNameLabel->setText(userName);
}

QSize NinjamTrackGroupView::sizeHint() const
{
    if (tracksLayoutEnum == TracksLayout::VerticalLayout) {
        return TrackGroupView::sizeHint();
    }
    else if (tracksLayoutEnum == TracksLayout::HorizontalLayout) {
        int height = 0;
        for (auto trackView : trackViews) {
            if (trackView->isVisible()) {
                height += trackView->minimumSizeHint().height();
            }
        }

        auto margins = mainLayout->contentsMargins();
        return QSize(1, qMax(height, 64) + margins.top() + margins.bottom()); // 64 is the minimum height of a horizontal narrowed track
    }

    // grid layout
    return QSize(NinjamTrackGroupView::MAX_WIDTH_IN_GRID_LAYOUT, NinjamTrackGroupView::MAX_HEIGHT_IN_GRID_LAYOUT);
}

NinjamTrackView *NinjamTrackGroupView::addTrackView(long trackID)
{
    NinjamTrackView *newTrackView = dynamic_cast<NinjamTrackView *>(TrackGroupView::addTrackView(trackID));
    newTrackView->setOrientation(getTracksOrientation());
    return newTrackView;
}

Qt::Orientation NinjamTrackGroupView::getTracksOrientation() const
{
    if (tracksLayoutEnum == TracksLayout::VerticalLayout)
        return Qt::Vertical;

    if (tracksLayoutEnum == TracksLayout::HorizontalLayout)
        return Qt::Horizontal;

    return  Qt::Vertical;
}

void NinjamTrackGroupView::setNarrowStatus(bool narrow)
{
    auto setToWide = !narrow && trackViews.size() <= 1;

    for (auto trackView : trackViews) {
        if (setToWide)
            trackView->setToWide();
        else
            trackView->setToNarrow();
    }

    updateGeometry();
}

void NinjamTrackGroupView::updateGuiElements()
{
    TrackGroupView::updateGuiElements();
    userNameLabel->updateMarquee();
}

NinjamTrackGroupView::~NinjamTrackGroupView()
{
    //
}
