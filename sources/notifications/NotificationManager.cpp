/**
 * @file NotificationManager.cpp
 * @brief Desktop notifications with per-notification activation targets.
 */

#include "NotificationManager.h"

#include "TaskbarAttentionPolicy.h"

#include <algorithm>

#include <QApplication>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidget>

#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
#include <QVariantMap>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusInterface>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusReply>
#endif

namespace Mattermost {

namespace {

constexpr int IncomingCallDurationMs = 30000;

int notificationDisplayDurationMs(const QString& title, const QString& body)
{
    // Give a short notification enough time to register, while allowing a
    // normal message roughly the time needed to read and understand it. 14
    // characters/second is deliberately conservative for chat text; the extra
    // two seconds cover noticing the popup and switching attention to it.
    constexpr qint64 minimumMs = 5000;
    constexpr qint64 maximumMs = 12000;
    constexpr qint64 attentionMs = 2000;
    constexpr qint64 charactersPerSecond = 14;

    const qint64 characterCount = static_cast<qint64>(title.simplified().size())
        + static_cast<qint64>(body.simplified().size());
    const qint64 readingMs = attentionMs
        + (characterCount * 1000 + charactersPerSecond - 1) / charactersPerSecond;
    return static_cast<int>(std::clamp(readingMs, minimumMs, maximumMs));
}

#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
void closeFreedesktopNotification(uint id)
{
    auto message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("CloseNotification"));
    message << id;
    QDBusConnection::sessionBus().asyncCall(message);
}

bool sendLauncherUrgency(bool urgent)
{
    const QString desktopId = QGuiApplication::desktopFileName();
    if (desktopId.isEmpty()) {
        return false;
    }

    // Plasma's task manager listens to this launcher update independently
    // of KWin's window activation protocol.
    auto message = QDBusMessage::createSignal(
        QStringLiteral("/com/canonical/Unity/LauncherEntry"),
        QStringLiteral("com.canonical.Unity.LauncherEntry"),
        QStringLiteral("Update"));
    const QVariantMap properties {{QStringLiteral("urgent"), urgent}};
    message << QStringLiteral("application://%1.desktop").arg(desktopId) << properties;
    return QDBusConnection::sessionBus().send(message);
}
#endif

} // namespace

NotificationManager::NotificationManager(QSystemTrayIcon& trayIcon, QObject* parent)
    : QObject(parent)
#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    , freedesktopActionsSupported(false)
#endif
    , trayIcon(trayIcon)
{
    connect(&trayIcon, &QSystemTrayIcon::messageClicked,
            this, &NotificationManager::onFallbackMessageClicked);

#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            clearLauncherUrgency();
        }
    });

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }

    QDBusInterface notifications(QStringLiteral("org.freedesktop.Notifications"),
                                 QStringLiteral("/org/freedesktop/Notifications"),
                                 QStringLiteral("org.freedesktop.Notifications"), bus);
    if (!notifications.isValid()) {
        return;
    }

    const QDBusReply<QStringList> capabilities = notifications.call(QStringLiteral("GetCapabilities"));
    if (!capabilities.isValid()
        || !capabilities.value().contains(QStringLiteral("actions"))) {
        return;
    }

    const bool actionConnected = bus.connect(
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("/org/freedesktop/Notifications"),
        QStringLiteral("org.freedesktop.Notifications"),
        QStringLiteral("ActionInvoked"),
        this, SLOT(onActionInvoked(uint,QString)));
    if (!actionConnected) {
        return;
    }

    // NotificationClosed is only used for cleanup. A failure to subscribe to
    // it must not disable exact click handling; the remaining entries are tiny
    // and live only for this process lifetime.
    bus.connect(QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("/org/freedesktop/Notifications"),
                QStringLiteral("org.freedesktop.Notifications"),
                QStringLiteral("NotificationClosed"),
                this, SLOT(onNotificationClosed(uint,uint)));
    freedesktopActionsSupported = true;
#endif
}

NotificationManager::~NotificationManager()
{
#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    for (auto it = freedesktopTargets.cbegin(); it != freedesktopTargets.cend(); ++it) {
        if (!it->callId.isEmpty()) {
            closeFreedesktopNotification(it.key());
        }
    }
    clearLauncherUrgency();
#endif
}

void NotificationManager::show(const QString& title, const QString& body,
                               const NotificationTarget& target)
{
#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    if (freedesktopActionsSupported && showFreedesktop(title, body, target)) {
        return;
    }
#endif

    fallbackTarget = target;
    trayIcon.showMessage(title, body, QSystemTrayIcon::Information,
                         target.callId.isEmpty() ? notificationDisplayDurationMs(title, body)
                                                 : IncomingCallDurationMs);
}

void NotificationManager::requestTaskbarAttention(QWidget& window)
{
    if (!window.isVisible()) {
        return;
    }

    const auto attention = taskbarAttentionFor(QGuiApplication::platformName(),
                                               QGuiApplication::applicationState());
    if (attention == TaskbarAttention::NativeAlert) {
        QApplication::alert(&window);
    }
#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    if (attention == TaskbarAttention::LauncherUrgency && sendLauncherUrgency(true)) {
        launcherUrgent = true;
    }
#endif
}

void NotificationManager::closeIncomingCall(const QString& callId)
{
    if (callId.isEmpty()) {
        return;
    }
    if (fallbackTarget.callId == callId) {
        fallbackTarget = {};
    }
#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    for (auto it = freedesktopTargets.begin(); it != freedesktopTargets.end();) {
        if (it->callId != callId) {
            ++it;
            continue;
        }
        closeFreedesktopNotification(it.key());
        it = freedesktopTargets.erase(it);
    }
#endif
}

void NotificationManager::onFallbackMessageClicked()
{
    if (!fallbackTarget.isValid()) {
        return;
    }

    const NotificationTarget target = fallbackTarget;
    fallbackTarget = NotificationTarget {};
    emit activated(target);
}

#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
void NotificationManager::clearLauncherUrgency()
{
    if (launcherUrgent && sendLauncherUrgency(false)) {
        launcherUrgent = false;
    }
}

bool NotificationManager::showFreedesktop(const QString& title, const QString& body,
                                           const NotificationTarget& target)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return false;
    }

    QDBusInterface notifications(QStringLiteral("org.freedesktop.Notifications"),
                                 QStringLiteral("/org/freedesktop/Notifications"),
                                 QStringLiteral("org.freedesktop.Notifications"), bus);
    if (!notifications.isValid()) {
        return false;
    }

    QStringList actions;
    if (target.isValid()) {
        actions << QStringLiteral("default") << tr("Open");
    }
    if (!target.callId.isEmpty()) {
        actions << QStringLiteral("accept-call") << tr("Accept")
                << QStringLiteral("decline-call") << tr("Decline");
    }
    QVariantMap hints {
        {QStringLiteral("desktop-entry"),
         QStringLiteral("io.github.Ri0n.MatterLeast")},
    };
    if (!target.callId.isEmpty()) {
        hints.insert(QStringLiteral("suppress-sound"), true);
    }
    const QList<QVariant> arguments {
        qApp->applicationName(),
        QVariant::fromValue(uint(0)),
        QStringLiteral("io.github.Ri0n.MatterLeast"),
        title,
        target.callId.isEmpty() ? body : body.toHtmlEscaped(),
        actions,
        hints,
        target.callId.isEmpty() ? notificationDisplayDurationMs(title, body)
                                : IncomingCallDurationMs,
    };

    const QDBusMessage message = notifications.callWithArgumentList(
        QDBus::Block, QStringLiteral("Notify"), arguments);
    const QDBusReply<uint> reply(message);
    if (!reply.isValid()) {
        return false;
    }

    if (target.isValid()) {
        freedesktopTargets.insert(reply.value(), target);
    }
    return true;
}

void NotificationManager::onActionInvoked(uint notificationId, const QString& actionKey)
{
    const auto it = freedesktopTargets.constFind(notificationId);
    if (it == freedesktopTargets.cend()) {
        return;
    }

    const auto target = it.value();
    if (actionKey == QLatin1String("default")) {
        if (target.callId.isEmpty()) {
            freedesktopTargets.remove(notificationId);
        }
        emit activated(target);
    } else if (!target.callId.isEmpty()
               && (actionKey == QLatin1String("accept-call")
                   || actionKey == QLatin1String("decline-call"))) {
        emit incomingCallAnswered(target.callId, actionKey == QLatin1String("accept-call"));
    }
}

void NotificationManager::onNotificationClosed(uint notificationId, uint reason)
{
    Q_UNUSED(reason);

    // Some notification servers close a notification as part of invoking its
    // default action. The specification does not guarantee whether
    // NotificationClosed or ActionInvoked is delivered first, so retain the
    // target briefly to let a following ActionInvoked resolve the same ID.
    QTimer::singleShot(5000, this, [this, notificationId] {
        freedesktopTargets.remove(notificationId);
    });
}
#endif

} // namespace Mattermost
