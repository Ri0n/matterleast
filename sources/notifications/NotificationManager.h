/**
 * @file NotificationManager.h
 * @brief Desktop notifications with per-notification activation targets.
 */

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>

#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
#include <QMap>
#endif

class QSystemTrayIcon;
class QWidget;

namespace Mattermost {

struct NotificationTarget {
    QString channelId;
    QString postId;
    QString rootId;
    QString callId = {};
    // The exact originating connection; QPointer invalidates delayed clicks
    // when that session disappears, even if its remote IDs are reused.
    QPointer<QObject> accountSession;

    bool isValid() const
    {
        return accountSession && !channelId.isEmpty() && !postId.isEmpty();
    }
};

class NotificationManager : public QObject {
    Q_OBJECT
public:
    static NotificationManager& instance(QSystemTrayIcon& trayIcon);
    ~NotificationManager() override;

    void show(const QString& title, const QString& body, const NotificationTarget& target);
    void closeIncomingCall(QObject& accountSession, const QString& callId);
    void clearAccountSession(QObject& accountSession);
    void requestTaskbarAttention(QWidget& window);

signals:
    void activated(const NotificationTarget& target);
    void incomingCallAnswered(const NotificationTarget& target, bool accept);

private slots:
    void onFallbackMessageClicked();

#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    void onActionInvoked(uint notificationId, const QString& actionKey);
    void onNotificationClosed(uint notificationId, uint reason);
#endif

private:
    explicit NotificationManager(QSystemTrayIcon& trayIcon);
#ifdef MATTERMOST_QT_FREEDESKTOP_NOTIFICATIONS
    bool showFreedesktop(const QString& title, const QString& body,
                         const NotificationTarget& target);
    void clearLauncherUrgency();
    bool freedesktopActionsSupported;
    bool launcherUrgent = false;
    QMap<uint, NotificationTarget> freedesktopTargets;
#endif

    QSystemTrayIcon& trayIcon;
    NotificationTarget fallbackTarget;
};

} // namespace Mattermost
