#include <QtTest>
#include <QSystemTrayIcon>
#include <QVector>

#include "notifications/NotificationManager.h"

using namespace Mattermost;

namespace {
NotificationTarget targetFor(QObject& account, const QString& callId = {})
{
    // Remote identifiers intentionally collide on both accounts.
    return {QStringLiteral("channel"), QStringLiteral("post"),
            QStringLiteral("root"), callId, &account};
}
}

class NotificationMessageOwnerTest final : public QObject
{
    Q_OBJECT
private slots:
    void clickResolvesOnlyLatestServersPopup()
    {
        QSystemTrayIcon tray;
        QObject firstAccount;
        QObject secondAccount;
        auto& manager = NotificationManager::instance(tray);
        QCOMPARE(&NotificationManager::instance(tray), &manager);
        QVector<NotificationTarget> clicked;
        connect(&manager, &NotificationManager::activated, &tray,
                [&clicked](const NotificationTarget& target) { clicked.append(target); });

        manager.show("A", "first", targetFor(firstAccount));
        manager.show("B", "second", targetFor(secondAccount));
        emit tray.messageClicked();
        QCOMPARE(clicked.size(), 1);
        QCOMPARE(clicked.front().accountSession.data(), &secondAccount);
        emit tray.messageClicked();
        QCOMPARE(clicked.size(), 1);
    }

    void destroyedSessionCannotLeaveActionableTarget()
    {
        QSystemTrayIcon tray;
        auto& manager = NotificationManager::instance(tray);
        int clicked = 0;
        connect(&manager, &NotificationManager::activated, &tray,
                [&clicked](const NotificationTarget&) { ++clicked; });
        auto* account = new QObject;
        manager.show("A", "first", targetFor(*account));
        delete account;
        emit tray.messageClicked();
        QCOMPARE(clicked, 0);
    }

    void closingAnAccountOrCallDoesNotClearAnotherAccountsPopup()
    {
        QSystemTrayIcon tray;
        QObject firstAccount;
        QObject secondAccount;
        auto& manager = NotificationManager::instance(tray);
        const QString callId = QStringLiteral("same-call");
        QVector<NotificationTarget> clicked;
        connect(&manager, &NotificationManager::activated, &tray,
                [&clicked](const NotificationTarget& target) { clicked.append(target); });

        manager.show("B", "call", targetFor(secondAccount, callId));
        manager.closeIncomingCall(firstAccount, callId);
        manager.clearAccountSession(firstAccount);
        emit tray.messageClicked();
        QCOMPARE(clicked.size(), 1);
        QCOMPARE(clicked.front().accountSession.data(), &secondAccount);

        manager.show("B", "call", targetFor(secondAccount, callId));
        manager.closeIncomingCall(secondAccount, callId);
        emit tray.messageClicked();
        QCOMPARE(clicked.size(), 1);

        manager.show("B", "post", targetFor(secondAccount));
        manager.clearAccountSession(secondAccount);
        emit tray.messageClicked();
        QCOMPARE(clicked.size(), 1);
    }
};

QTEST_MAIN(NotificationMessageOwnerTest)
#include "NotificationMessageOwnerTest.moc"
