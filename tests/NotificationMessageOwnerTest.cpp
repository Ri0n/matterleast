#include <QtTest>
#include <QPointer>

#include "notifications/TrayFallbackOwner.h"

using namespace Mattermost;

class NotificationMessageOwnerTest final : public QObject
{
    Q_OBJECT
private slots:
    void clickResolvesOnlyLatestServersPopup()
    {
        QObject tray;
        TrayFallbackOwner owner(&tray);
        QObject firstAccount;
        QObject secondAccount;

        owner.claim(&firstAccount);
        QCOMPARE(owner.currentOwner(), &firstAccount);
        owner.claim(&secondAccount);
        QCOMPARE(owner.currentOwner(), &secondAccount);

        // All live account handlers receive messageClicked(). Only the
        // latest fallback popup owner is allowed to consume the click.
        QVERIFY(!owner.consume(&firstAccount));
        QVERIFY(owner.consume(&secondAccount));
        QVERIFY(!owner.consume(&secondAccount));
        QCOMPARE(owner.currentOwner(), nullptr);
    }

    void destroyingOldManagerNeverStealsCurrentPopup()
    {
        QObject tray;
        TrayFallbackOwner owner(&tray);
        auto* oldAccount = new QObject;
        QObject liveAccount;
        owner.claim(oldAccount);
        owner.claim(&liveAccount);
        delete oldAccount;
        QCOMPARE(owner.currentOwner(), &liveAccount);
        QVERIFY(owner.consume(&liveAccount));
    }

    void destroyedLatestOwnerCannotLeaveDanglingTarget()
    {
        QObject tray;
        TrayFallbackOwner owner(&tray);
        auto* account = new QObject;
        owner.claim(account);
        delete account;
        QCOMPARE(owner.currentOwner(), nullptr);
        QObject replacement;
        owner.claim(&replacement);
        QVERIFY(owner.consume(&replacement));
    }

    void releaseDoesNotClearNewerPopup()
    {
        QObject tray;
        TrayFallbackOwner owner(&tray);
        QObject firstAccount;
        QObject secondAccount;
        owner.claim(&firstAccount);
        owner.claim(&secondAccount);
        owner.release(&firstAccount);
        QCOMPARE(owner.currentOwner(), &secondAccount);
        owner.release(&secondAccount);
        QCOMPARE(owner.currentOwner(), nullptr);
    }
};

QTEST_GUILESS_MAIN(NotificationMessageOwnerTest)
#include "NotificationMessageOwnerTest.moc"
