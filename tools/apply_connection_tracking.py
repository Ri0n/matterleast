#!/usr/bin/env python3
from pathlib import Path


def read(path):
    return Path(path).read_text()


def write(path, text):
    Path(path).write_text(text)


def replace_once(path, old, new):
    text = read(path)
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{path}: expected one occurrence, got {count}: {old[:80]!r}")
    write(path, text.replace(old, new, 1))


# Shared compact spinner painter: ThemeIconButton and the avatar badge use the
# same timing/arc geometry, only with different bounds and pen widths.
write("sources/ui/BusyIndicator.h", r'''/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#pragma once

#include <QColor>
#include <QPainter>

namespace Mattermost::BusyIndicator {

inline constexpr int AnimationIntervalMs = 70;
inline constexpr int AnimationSteps = 12;

inline void draw(QPainter& painter,
                 const QRectF& ring,
                 int phase,
                 const QColor& color,
                 qreal penWidth = 2.0)
{
    if (ring.isEmpty()) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(color, penWidth, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    const int normalizedPhase = ((phase % AnimationSteps) + AnimationSteps) % AnimationSteps;
    painter.drawArc(ring, (-90 + normalizedPhase * 30) * 16, 105 * 16);
    painter.restore();
}

} // namespace Mattermost::BusyIndicator
''')

# Reuse the shared spinner from the composer button.
replace_once(
    "sources/ui/ThemeIconWidgets.cpp",
    '#include "IconUtils.h"\n',
    '#include "BusyIndicator.h"\n#include "IconUtils.h"\n')
replace_once(
    "sources/ui/ThemeIconWidgets.cpp",
    'constexpr int BusyAnimationIntervalMs = 70;\nconstexpr int BusyAnimationSteps = 12;\nconstexpr int BusyIndicatorExtent = 18;\n',
    'constexpr int BusyIndicatorExtent = 18;\n')
replace_once(
    "sources/ui/ThemeIconWidgets.cpp",
    '    _busyAnimationTimer.setInterval(BusyAnimationIntervalMs);\n'
    '    connect(&_busyAnimationTimer, &QTimer::timeout, this, [this] {\n'
    '        _busyPhase = (_busyPhase + 1) % BusyAnimationSteps;\n',
    '    _busyAnimationTimer.setInterval(BusyIndicator::AnimationIntervalMs);\n'
    '    connect(&_busyAnimationTimer, &QTimer::timeout, this, [this] {\n'
    '        _busyPhase = (_busyPhase + 1) % BusyIndicator::AnimationSteps;\n')
replace_once(
    "sources/ui/ThemeIconWidgets.cpp",
    '''        painter.setPen(QPen(busyColor, 2.0, Qt::SolidLine, Qt::RoundCap));
        painter.setBrush(Qt::NoBrush);

        const qreal indicatorExtent = BusyIndicatorExtent;
        const QRectF ring((width() - indicatorExtent) / 2.0 + 2.5,
                          (height() - indicatorExtent) / 2.0 + 2.5,
                          indicatorExtent - 5.0,
                          indicatorExtent - 5.0);
        painter.drawArc(ring, (-90 + _busyPhase * 30) * 16, 105 * 16);
''',
    '''        const qreal indicatorExtent = BusyIndicatorExtent;
        const QRectF ring((width() - indicatorExtent) / 2.0 + 2.5,
                          (height() - indicatorExtent) / 2.0 + 2.5,
                          indicatorExtent - 5.0,
                          indicatorExtent - 5.0);
        BusyIndicator::draw(painter, ring, _busyPhase, busyColor, 2.0);
''')

# Presence avatar owns a smaller instance of the same animation and only
# intercepts clicks inside the badge hit area.
write("sources/ui/PresenceAvatarLabel.h", r'''/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#pragma once

#include <QColor>
#include <QPixmap>
#include <QRectF>
#include <QString>
#include <QTimer>

#include "ClickableLabel.h"

class QEvent;
class QMouseEvent;
class QPaintEvent;
class QResizeEvent;

namespace Mattermost {

/**
 * QLabel-compatible avatar that renders presence and connection state in the
 * lower-right badge. During initial connection/reconnect the presence badge is
 * replaced by the same compact spinner used by other busy UI actions.
 */
class PresenceAvatarLabel final : public ClickableLabel
{
    Q_OBJECT

public:
    enum class ConnectionIndicatorState {
        None,
        Connecting,
        WaitingForReconnect,
    };
    Q_ENUM(ConnectionIndicatorState)

    explicit PresenceAvatarLabel(QWidget* parent = nullptr);

    void setPixmap(const QPixmap& pixmap);
    void setStatus(const QString& status);
    void setConnectionIndicatorState(ConnectionIndicatorState state);
    ConnectionIndicatorState connectionIndicatorState() const { return connectionState; }

    static bool isPresenceStatus(const QString& text);

signals:
    void reconnectRequested();

protected:
    void changeEvent(QEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    QRectF connectionBadgeRect() const;
    void refreshPixmap();

    QPixmap sourcePixmap;
    QString presenceStatus;
    QColor renderedBackground;
    QTimer connectionAnimationTimer;
    ConnectionIndicatorState connectionState = ConnectionIndicatorState::None;
    int connectionAnimationPhase = 0;
};

/**
 * Compatibility label for the sidebar header. Existing MainWindow code writes
 * online/away/dnd/offline as text; consume that value as avatar state instead
 * of displaying a second textual presence indicator.
 */
class PresenceStatusLabel final : public QLabel
{
    Q_OBJECT

public:
    explicit PresenceStatusLabel(QWidget* parent = nullptr);

    void setText(const QString& text);
};

} // namespace Mattermost
''')

write("sources/ui/PresenceAvatarLabel.cpp", r'''/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#include "PresenceAvatarLabel.h"

#include <algorithm>

#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QResizeEvent>

#include "AvatarUtils.h"
#include "BusyIndicator.h"

namespace Mattermost {

namespace {

constexpr int DefaultAvatarSize = 48;
constexpr int DefaultBadgeSize = 12;
constexpr qreal BadgeHitMargin = 3.0;

QColor applicationWindowColor()
{
    return qApp ? qApp->palette().color(QPalette::Window) : QColor();
}

} // namespace

PresenceAvatarLabel::PresenceAvatarLabel(QWidget* parent)
    : ClickableLabel(parent)
{
    setFrameShape(QFrame::NoFrame);
    setAlignment(Qt::AlignCenter);

    connectionAnimationTimer.setInterval(BusyIndicator::AnimationIntervalMs);
    connect(&connectionAnimationTimer, &QTimer::timeout, this, [this] {
        connectionAnimationPhase =
            (connectionAnimationPhase + 1) % BusyIndicator::AnimationSteps;
        update();
    });
}

void PresenceAvatarLabel::setPixmap(const QPixmap& pixmap)
{
    sourcePixmap = pixmap;
    refreshPixmap();
}

void PresenceAvatarLabel::setStatus(const QString& status)
{
    if (presenceStatus == status) {
        return;
    }
    presenceStatus = status;
    refreshPixmap();
}

void PresenceAvatarLabel::setConnectionIndicatorState(ConnectionIndicatorState state)
{
    if (connectionState == state) {
        return;
    }

    connectionState = state;
    if (connectionState == ConnectionIndicatorState::None) {
        connectionAnimationTimer.stop();
        connectionAnimationPhase = 0;
        setToolTip(QString());
    } else {
        if (!connectionAnimationTimer.isActive()) {
            connectionAnimationTimer.start();
        }
        setToolTip(connectionState == ConnectionIndicatorState::WaitingForReconnect
                       ? tr("Reconnecting to Mattermost… Click the indicator to retry now.")
                       : tr("Connecting to Mattermost…"));
    }

    refreshPixmap();
    update();
}

bool PresenceAvatarLabel::isPresenceStatus(const QString& text)
{
    return text == QStringLiteral("online")
        || text == QStringLiteral("away")
        || text == QStringLiteral("dnd")
        || text == QStringLiteral("offline");
}

void PresenceAvatarLabel::changeEvent(QEvent* event)
{
    ClickableLabel::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange)) {
        refreshPixmap();
    }
}

void PresenceAvatarLabel::mouseReleaseEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::LeftButton
        && connectionState != ConnectionIndicatorState::None
        && !sourcePixmap.isNull()
        && connectionBadgeRect().adjusted(-BadgeHitMargin, -BadgeHitMargin,
                                          BadgeHitMargin, BadgeHitMargin)
               .contains(event->pos())) {
        QLabel::mouseReleaseEvent(event);
        if (connectionState == ConnectionIndicatorState::WaitingForReconnect) {
            emit reconnectRequested();
        }
        return;
    }

    ClickableLabel::mouseReleaseEvent(event);
}

void PresenceAvatarLabel::paintEvent(QPaintEvent* event)
{
    // The badge contains a one-pixel background ring. Validate that cached
    // pixmap at the actual paint boundary so a desktop palette transition can
    // never leave that ring in the previous theme's background colour.
    const QColor currentBackground = applicationWindowColor();
    if (renderedBackground != currentBackground) {
        refreshPixmap();
    }
    ClickableLabel::paintEvent(event);

    if (connectionState == ConnectionIndicatorState::None || sourcePixmap.isNull()) {
        return;
    }

    QPainter painter(this);
    const QRectF badge = connectionBadgeRect();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(renderedBackground);
    painter.drawEllipse(badge.adjusted(-1.0, -1.0, 1.0, 1.0));

    QColor spinnerColor = (qApp ? qApp->palette() : palette()).color(QPalette::WindowText);
    spinnerColor.setAlpha(190);
    const qreal inset = std::max<qreal>(1.5, badge.width() / 6.0);
    const qreal penWidth = std::max<qreal>(1.2, badge.width() / 7.0);
    BusyIndicator::draw(painter, badge.adjusted(inset, inset, -inset, -inset),
                        connectionAnimationPhase, spinnerColor, penWidth);
}

void PresenceAvatarLabel::resizeEvent(QResizeEvent* event)
{
    ClickableLabel::resizeEvent(event);
    refreshPixmap();
}

QRectF PresenceAvatarLabel::connectionBadgeRect() const
{
    int avatarSize = std::min(width(), height());
    if (avatarSize <= 0) {
        avatarSize = DefaultAvatarSize;
    }
    const int badgeSize = std::max(4,
        qRound(DefaultBadgeSize * avatarSize / static_cast<qreal>(DefaultAvatarSize)));
    const qreal avatarLeft = (width() - avatarSize) / 2.0;
    const qreal avatarTop = (height() - avatarSize) / 2.0;
    return QRectF(avatarLeft + avatarSize - badgeSize - 1,
                  avatarTop + avatarSize - badgeSize - 1,
                  badgeSize,
                  badgeSize);
}

void PresenceAvatarLabel::refreshPixmap()
{
    if (sourcePixmap.isNull()) {
        renderedBackground = applicationWindowColor();
        QLabel::clear();
        return;
    }

    int avatarSize = std::min(width(), height());
    if (avatarSize <= 0) {
        avatarSize = DefaultAvatarSize;
    }
    const int badgeSize = std::max(4,
        qRound(DefaultBadgeSize * avatarSize / static_cast<qreal>(DefaultAvatarSize)));

    renderedBackground = applicationWindowColor();
    if (connectionState == ConnectionIndicatorState::None) {
        QLabel::setPixmap(AvatarUtils::withStatus(sourcePixmap,
                                                   avatarSize,
                                                   presenceStatus,
                                                   badgeSize,
                                                   renderedBackground));
    } else {
        QLabel::setPixmap(AvatarUtils::circular(sourcePixmap, avatarSize));
    }
}

PresenceStatusLabel::PresenceStatusLabel(QWidget* parent)
    : QLabel(parent)
{
}

void PresenceStatusLabel::setText(const QString& text)
{
    if (PresenceAvatarLabel::isPresenceStatus(text)) {
        if (QWidget* host = parentWidget()) {
            if (auto* avatar = host->findChild<PresenceAvatarLabel*>(
                    QStringLiteral("usericon_label"))) {
                avatar->setStatus(text);
                QLabel::clear();
                hide();
                return;
            }
        }
    }

    QLabel::setText(text);
    setVisible(!text.isEmpty());
}

} // namespace Mattermost
''')

# Explicit realtime connection state and a public immediate-retry entry point.
replace_once(
    "sources/backend/WebSocketConnector.h",
    '''public:
\tWebSocketConnector (WebSocketEventHandler& eventHandler);
\t~WebSocketConnector () override;
public:
\tvoid open (const QString& urlString, const QString& authToken);
\tvoid close ();
\tvoid reset ();
\tvoid doHandshake ();
signals:
\tvoid onConnect (bool isReconnect);
\tvoid onDisconnect ();
private:
''',
    '''public:
    enum class ConnectionState {
        Disconnected,
        Connecting,
        Connected,
        WaitingForReconnect,
    };
    Q_ENUM(ConnectionState)

\tWebSocketConnector (WebSocketEventHandler& eventHandler);
\t~WebSocketConnector () override;
public:
\tvoid open (const QString& urlString, const QString& authToken);
\tvoid close ();
\tvoid reset ();
\tvoid doHandshake ();
    ConnectionState connectionState() const;
    void reconnectNow();
signals:
\tvoid onConnect (bool isReconnect);
\tvoid onDisconnect ();
    void connectionStateChanged(ConnectionState state);
private:
    void setConnectionState(ConnectionState state);
''')

replace_once(
    "sources/backend/WebSocketConnector.cpp",
    '#include <QNetworkRequest>\n#include <QRandomGenerator>\n',
    '#include <QNetworkRequest>\n#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)\n#include <QNetworkInformation>\n#endif\n#include <QRandomGenerator>\n')
replace_once(
    "sources/backend/WebSocketConnector.cpp",
    '\tbool suppressReconnect = false;\n};\n',
    '\tbool suppressReconnect = false;\n    ConnectionState connectionState = ConnectionState::Disconnected;\n};\n')
replace_once(
    "sources/backend/WebSocketConnector.cpp",
    '''\td->reconnectTimer.setSingleShot (true);
\tconnect (&d->reconnectTimer, &QTimer::timeout, this, [this] {
\t\tif (d->token.isEmpty() || d->suppressReconnect) {
\t\t\treturn;
\t\t}
\t\tif (d->webSocket.state() != QAbstractSocket::UnconnectedState) {
\t\t\treturn;
\t\t}

\t\tLOG_DEBUG ("WebSocket Reconnecting (connection_id=" << d->connectionId
\t\t\t\t   << ", sequence_number=" << d->serverSequence << ")");
\t\topenSocket ();
\t});
}

WebSocketConnector::~WebSocketConnector () = default;
''',
    '''\td->reconnectTimer.setSingleShot (true);
\tconnect (&d->reconnectTimer, &QTimer::timeout, this, [this] {
\t\tif (d->token.isEmpty() || d->suppressReconnect) {
\t\t\treturn;
\t\t}
\t\tif (d->webSocket.state() != QAbstractSocket::UnconnectedState) {
\t\t\treturn;
\t\t}

\t\tLOG_DEBUG ("WebSocket Reconnecting (connection_id=" << d->connectionId
\t\t\t\t   << ", sequence_number=" << d->serverSequence << ")");
\t\topenSocket ();
\t});

#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    if (QNetworkInformation::instance() || QNetworkInformation::loadDefaultBackend()) {
        if (QNetworkInformation* networkInformation = QNetworkInformation::instance()) {
            connect(networkInformation, &QNetworkInformation::reachabilityChanged,
                    this, [this](QNetworkInformation::Reachability) {
                if (d->connectionState == ConnectionState::WaitingForReconnect) {
                    LOG_DEBUG("Network reachability changed while waiting to reconnect");
                    reconnectNow();
                }
            });
            connect(networkInformation, &QNetworkInformation::transportMediumChanged,
                    this, [this](QNetworkInformation::TransportMedium) {
                if (d->connectionState == ConnectionState::WaitingForReconnect) {
                    LOG_DEBUG("Network transport changed while waiting to reconnect");
                    reconnectNow();
                }
            });
        }
    }
#endif
}

WebSocketConnector::~WebSocketConnector () = default;

WebSocketConnector::ConnectionState WebSocketConnector::connectionState() const
{
    return d->connectionState;
}

void WebSocketConnector::setConnectionState(ConnectionState state)
{
    if (d->connectionState == state) {
        return;
    }
    d->connectionState = state;
    emit connectionStateChanged(state);
}

void WebSocketConnector::reconnectNow()
{
    if (d->connectionState != ConnectionState::WaitingForReconnect
        || d->token.isEmpty() || d->suppressReconnect
        || d->webSocket.state() != QAbstractSocket::UnconnectedState) {
        return;
    }

    d->reconnectTimer.stop();
    LOG_DEBUG("WebSocket reconnect requested immediately (connection_id="
              << d->connectionId << ", sequence_number=" << d->serverSequence << ")");
    openSocket();
}
''')
replace_once(
    "sources/backend/WebSocketConnector.cpp",
    '\td->reconnectTimer.stop ();\n\tstopHeartbeat ();\n\n\tconst quint64 generation',
    '\td->reconnectTimer.stop ();\n\tstopHeartbeat ();\n    d->endpointUrl.clear();\n    setConnectionState(ConnectionState::Connecting);\n\n\tconst quint64 generation')
replace_once(
    "sources/backend/WebSocketConnector.cpp",
    '\tLOG_DEBUG ("WebSocket reconnect scheduled in " << delay << " ms");\n\td->reconnectTimer.start (delay);\n',
    '\tLOG_DEBUG ("WebSocket reconnect scheduled in " << delay << " ms");\n    setConnectionState(ConnectionState::WaitingForReconnect);\n\td->reconnectTimer.start (delay);\n')
replace_once(
    "sources/backend/WebSocketConnector.cpp",
    '''\tconst QUrl url = socketUrl ();
\tLOG_DEBUG ("WebSocket opening " << url.toString(QUrl::RemovePassword));
''',
    '''    setConnectionState(ConnectionState::Connecting);
\tconst QUrl url = socketUrl ();
\tLOG_DEBUG ("WebSocket opening " << url.toString(QUrl::RemovePassword));
''')
replace_once(
    "sources/backend/WebSocketConnector.cpp",
    '\td->helloReceived = false;\n\n\tif (d->webSocket.state() != QAbstractSocket::UnconnectedState) {',
    '\td->helloReceived = false;\n    setConnectionState(ConnectionState::Disconnected);\n\n\tif (d->webSocket.state() != QAbstractSocket::UnconnectedState) {')
replace_once(
    "sources/backend/WebSocketConnector.cpp",
    '''\t\tconst bool needsHttpResync = d->hasReconnect && d->resumeFailed;
\t\td->hasReconnect = false;
\t\td->resumeFailed = false;
\t\temit onConnect (needsHttpResync);
''',
    '''\t\tconst bool needsHttpResync = d->hasReconnect && d->resumeFailed;
\t\td->hasReconnect = false;
\t\td->resumeFailed = false;
        setConnectionState(ConnectionState::Connected);
\t\temit onConnect (needsHttpResync);
''')
replace_once(
    "sources/backend/WebSocketConnector.cpp",
    '''\t\tif (!reconnectSuppressed && !d->token.isEmpty()) {
\t\t\tscheduleReconnect ();
\t\t}
''',
    '''\t\tif (!reconnectSuppressed && !d->token.isEmpty()) {
\t\t\tscheduleReconnect ();
        } else {
            setConnectionState(ConnectionState::Disconnected);
\t\t}
''')

# Backend proxies the state without exposing its connector object.
replace_once(
    "sources/backend/Backend.h",
    '''\tBackendChannel* getCurrentChannel () const;

\t// A DM/GM has no intrinsic team''',
    '''\tBackendChannel* getCurrentChannel () const;

    WebSocketConnector::ConnectionState webSocketConnectionState() const;
    void retryWebSocketConnectionNow();

\t// A DM/GM has no intrinsic team''')
replace_once(
    "sources/backend/Backend.h",
    '''    void onWebSocketConnect ();
    void onWebSocketDisconnect ();
''',
    '''    void onWebSocketConnect ();
    void onWebSocketDisconnect ();
    void onWebSocketConnectionStateChanged(WebSocketConnector::ConnectionState state);
''')
replace_once(
    "sources/backend/Backend.cpp",
    '''\t//these signals are proxied
\tconnect (&webSocketConnector, &WebSocketConnector::onDisconnect, this, &Backend::onWebSocketDisconnect);
''',
    '''\t//these signals are proxied
\tconnect (&webSocketConnector, &WebSocketConnector::onDisconnect, this, &Backend::onWebSocketDisconnect);
    connect(&webSocketConnector, &WebSocketConnector::connectionStateChanged,
            this, &Backend::onWebSocketConnectionStateChanged);
''')
replace_once(
    "sources/backend/Backend.cpp",
    '''BackendChannel* Backend::getCurrentChannel () const
{
\treturn currentChannel;
}

bool Backend::autoLoginEnabled ()
''',
    '''BackendChannel* Backend::getCurrentChannel () const
{
\treturn currentChannel;
}

WebSocketConnector::ConnectionState Backend::webSocketConnectionState() const
{
    return webSocketConnector.connectionState();
}

void Backend::retryWebSocketConnectionNow()
{
    webSocketConnector.reconnectNow();
}

bool Backend::autoLoginEnabled ()
''')

# Sidebar connection indicator follows current state immediately, including the
# state that was emitted before MainWindow existed during login.
replace_once(
    "sources/MainWindowRealtime.cpp",
    '#include "ui/TeamSelectorLabel.h"\n',
    '#include "ui/PresenceAvatarLabel.h"\n#include "ui/TeamSelectorLabel.h"\n')
replace_once(
    "sources/MainWindowRealtime.cpp",
    '''    setProperty(InstalledProperty, true);

    // Active webapp plugins''',
    '''    setProperty(InstalledProperty, true);

    auto updateConnectionIndicator = [this](WebSocketConnector::ConnectionState state) {
        using IndicatorState = PresenceAvatarLabel::ConnectionIndicatorState;
        IndicatorState indicatorState = IndicatorState::None;
        if (state == WebSocketConnector::ConnectionState::Connecting) {
            indicatorState = IndicatorState::Connecting;
        } else if (state == WebSocketConnector::ConnectionState::WaitingForReconnect) {
            indicatorState = IndicatorState::WaitingForReconnect;
        }
        ui->usericon_label->setConnectionIndicatorState(indicatorState);
    };
    connect(&backend, &Backend::onWebSocketConnectionStateChanged,
            this, updateConnectionIndicator);
    connect(ui->usericon_label, &PresenceAvatarLabel::reconnectRequested,
            this, [this] { backend.retryWebSocketConnectionNow(); });
    updateConnectionIndicator(backend.webSocketConnectionState());

    // Active webapp plugins''')

# UI regression test: indicator replaces presence, animates, only badge clicks
# request reconnect, and ordinary avatar clicks remain profile clicks.
write("tests/PresenceAvatarLabelTest.cpp", r'''#include <QtTest>

#include <QImage>
#include <QPainter>
#include <QSignalSpy>

#include "ui/PresenceAvatarLabel.h"

using namespace Mattermost;

namespace {

QImage renderLabel(PresenceAvatarLabel& label)
{
    QImage image(label.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    label.render(&painter);
    return image;
}

} // namespace

class PresenceAvatarLabelTest : public QObject
{
    Q_OBJECT

private slots:
    void connectionIndicatorAnimatesAndRestoresPresence()
    {
        PresenceAvatarLabel label;
        label.setFixedSize(48, 48);
        QPixmap avatar(48, 48);
        avatar.fill(QColor(QStringLiteral("#607D8B")));
        label.setPixmap(avatar);
        label.setStatus(QStringLiteral("online"));
        label.show();
        QCoreApplication::processEvents();

        const QImage presence = renderLabel(label);
        label.setConnectionIndicatorState(
            PresenceAvatarLabel::ConnectionIndicatorState::Connecting);
        const QImage firstFrame = renderLabel(label);
        QVERIFY(firstFrame != presence);

        QTest::qWait(80);
        const QImage secondFrame = renderLabel(label);
        QVERIFY(secondFrame != firstFrame);

        label.setConnectionIndicatorState(
            PresenceAvatarLabel::ConnectionIndicatorState::None);
        QCOMPARE(renderLabel(label), presence);
    }

    void reconnectRequestOnlyComesFromWaitingBadge()
    {
        PresenceAvatarLabel label;
        label.setFixedSize(48, 48);
        QPixmap avatar(48, 48);
        avatar.fill(Qt::gray);
        label.setPixmap(avatar);
        label.show();
        QCoreApplication::processEvents();

        QSignalSpy reconnectSpy(&label, &PresenceAvatarLabel::reconnectRequested);
        QSignalSpy avatarClickSpy(&label, &ClickableLabel::clicked);

        label.setConnectionIndicatorState(
            PresenceAvatarLabel::ConnectionIndicatorState::Connecting);
        QTest::mouseClick(&label, Qt::LeftButton, Qt::NoModifier, QPoint(42, 42));
        QCOMPARE(reconnectSpy.count(), 0);
        QCOMPARE(avatarClickSpy.count(), 0);

        label.setConnectionIndicatorState(
            PresenceAvatarLabel::ConnectionIndicatorState::WaitingForReconnect);
        QTest::mouseClick(&label, Qt::LeftButton, Qt::NoModifier, QPoint(42, 42));
        QCOMPARE(reconnectSpy.count(), 1);
        QCOMPARE(avatarClickSpy.count(), 0);

        QTest::mouseClick(&label, Qt::LeftButton, Qt::NoModifier, QPoint(10, 10));
        QCOMPARE(reconnectSpy.count(), 1);
        QCOMPARE(avatarClickSpy.count(), 1);
    }
};

QTEST_MAIN(PresenceAvatarLabelTest)
#include "PresenceAvatarLabelTest.moc"
''')

replace_once(
    "tests/CMakeLists.txt",
    '''add_test(NAME theme-icon-widgets-test COMMAND theme-icon-widgets-test)
set_tests_properties(theme-icon-widgets-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(sidebar-channel-move-policy-test SidebarChannelMovePolicyTest.cpp)
''',
    '''add_test(NAME theme-icon-widgets-test COMMAND theme-icon-widgets-test)
set_tests_properties(theme-icon-widgets-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(presence-avatar-label-test
    PresenceAvatarLabelTest.cpp
    "${CMAKE_SOURCE_DIR}/sources/ui/PresenceAvatarLabel.cpp"
    "${CMAKE_SOURCE_DIR}/sources/ui/ClickableLabel.cpp"
)
target_include_directories(presence-avatar-label-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(presence-avatar-label-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Widgets)
add_test(NAME presence-avatar-label-test COMMAND presence-avatar-label-test)
set_tests_properties(presence-avatar-label-test PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

add_executable(sidebar-channel-move-policy-test SidebarChannelMovePolicyTest.cpp)
''')

# One-shot helper; the workflow that invokes this script is temporary too.
Path("tools/apply_connection_tracking.py").unlink()
workflow = Path(".github/workflows/apply-connection-tracking.yml")
if workflow.exists():
    workflow.unlink()
