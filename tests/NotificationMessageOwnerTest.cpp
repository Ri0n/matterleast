#include <QtTest>

#include <QSystemTrayIcon>

#include "notifications/NotificationManager.h"

using namespace Mattermost;

class NotificationMessageOwnerTest final : public QObject
{
    Q_OBJECT
private slots:
    void clickResolvesOnlyLatestServersPopup()
    {
        QSystemTrayIcon tray;
        NotificationManager serverOne(tray);
        NotificationManager serverTwo(tray);
        int oneClicks = 0;
        int twoClicks = 0;
        QString openedPost;
        connect(&serverOne, &NotificationManager::activated,
                &serverOne, [&](const NotificationTarget&) { ++oneClicks; });
        connect(&serverTwo, &NotificationManager::activated,
                &serverTwo, [&](const NotificationTarget& target) {
            ++twoClicks;
            openedPost = target.postId;
        });

        serverOne.show(QStringLiteral("First"), QStringLiteral("Message"),
                       {QStringLiteral("channel"), QStringLiteral("old-post"), {}});
        serverTwo.show(QStringLiteral("Second"), QStringLiteral("Message"),
                       {QStringLiteral("channel"), QStringLiteral("new-post"), {}});
        QVERIFY(QMetaObject::invokeMethod(&tray, "messageClicked",
                                          Qt::DirectConnection));
        QCOMPARE(oneClicks, 0);
        QCOMPARE(twoClicks, 1);
        QCOMPARE(openedPost, QStringLiteral("new-post"));
        QVERIFY(QMetaObject::invokeMethod(&tray, "messageClicked",
                                          Qt::DirectConnection));
        QCOMPARE(twoClicks, 1); // Consumed once.
    }

    void destroyingOldManagerNeverStealsCurrentPopup()
    {
        QSystemTrayIcon tray;
        auto* oldServer = new NotificationManager(tray);
        NotificationManager liveServer(tray);
        int delivered = 0;
        connect(&liveServer, &NotificationManager::activated,
                &liveServer, [&](const NotificationTarget&) { ++delivered; });

        oldServer->show(QStringLiteral("Old"), QStringLiteral("Message"),
                        {QStringLiteral("channel"), QStringLiteral("old"), {}});
        liveServer.show(QStringLiteral("Live"), QStringLiteral("Message"),
                        {QStringLiteral("channel"), QStringLiteral("new"), {}});
        delete oldServer;
        QVERIFY(QMetaObject::invokeMethod(&tray, "messageClicked",
                                          Qt::DirectConnection));
        QCOMPARE(delivered, 1);
    }
};

QTEST_MAIN(NotificationMessageOwnerTest)
#include "NotificationMessageOwnerTest.moc"
