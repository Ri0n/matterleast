#include <QtTest>

#include "notifications/PostNotificationReplayGuard.h"

using namespace Mattermost;

class PostNotificationReplayGuardTest final : public QObject {
    Q_OBJECT

private slots:
    void staleHistoryNeverProducesDesktopPopups()
    {
        constexpr quint64 now = 1780000000000ULL;
        PostNotificationReplayGuard guard;

        // A reconnect can replay posts from days or months ago, including
        // posts in the currently selected direct conversation.
        QVERIFY(!guard.shouldDeliver(QStringLiteral("month-old"),
                                     now - 30ULL * 24 * 60 * 60 * 1000, now));
        QVERIFY(!guard.shouldDeliver(QStringLiteral("day-old"),
                                     now - 24ULL * 60 * 60 * 1000, now));
        QVERIFY(!guard.shouldDeliver(QStringLiteral("just-too-old"),
                                     now - PostNotificationReplayGuard::MaxAgeMs - 1, now));
        QVERIFY(guard.shouldDeliver(QStringLiteral("boundary"),
                                    now - PostNotificationReplayGuard::MaxAgeMs, now));
        QVERIFY(guard.shouldDeliver(QStringLiteral("live"), now, now));
    }

    void missingAndFutureTimestampsAreRejected()
    {
        constexpr quint64 now = 1780000000000ULL;
        PostNotificationReplayGuard guard;
        QVERIFY(!guard.shouldDeliver(QStringLiteral("missing"), 0, now));
        QVERIFY(!guard.shouldDeliver(QString(), now, now));
        QVERIFY(!guard.shouldDeliver(QStringLiteral("badclock"), now, 0));
        QVERIFY(guard.shouldDeliver(QStringLiteral("minor-skew"),
                                    now + PostNotificationReplayGuard::AllowedFutureSkewMs, now));
        QVERIFY(!guard.shouldDeliver(QStringLiteral("future"),
                                     now + PostNotificationReplayGuard::AllowedFutureSkewMs + 1, now));
    }

    void resumedStreamCannotNotifyTheSamePostAgain()
    {
        constexpr quint64 now = 1780000000000ULL;
        PostNotificationReplayGuard guard;
        const QString id = QStringLiteral("post-in-open-dm");
        QVERIFY(guard.shouldDeliver(id, now - 2000, now));
        // The first event may have been suppressed because the DM was visible.
        // Replaying it after switching tabs must not create a new popup.
        QVERIFY(!guard.shouldDeliver(id, now - 2000, now + 1000));
        QVERIFY(guard.shouldDeliver(QStringLiteral("different-post"), now, now));
    }

    void rememberedIdsRemainBounded()
    {
        constexpr quint64 now = 1780000000000ULL;
        PostNotificationReplayGuard guard;
        QVERIFY(guard.shouldDeliver(QStringLiteral("oldest"), now, now));
        for (int i = 0; i < PostNotificationReplayGuard::MaxRememberedPosts; ++i) {
            QVERIFY(guard.shouldDeliver(QString::number(i), now, now));
        }
        // The guard is per window/session and cannot grow indefinitely.
        QVERIFY(guard.shouldDeliver(QStringLiteral("oldest"), now, now));
        QVERIFY(!guard.shouldDeliver(QString::number(
                    PostNotificationReplayGuard::MaxRememberedPosts - 1), now, now));
    }

    void differentSessionsHaveIndependentRecentIds()
    {
        constexpr quint64 now = 1780000000000ULL;
        PostNotificationReplayGuard first;
        PostNotificationReplayGuard second;
        QVERIFY(first.shouldDeliver(QStringLiteral("post"), now, now));
        QVERIFY(second.shouldDeliver(QStringLiteral("post"), now, now));
    }
};

QTEST_APPLESS_MAIN(PostNotificationReplayGuardTest)
#include "PostNotificationReplayGuardTest.moc"
