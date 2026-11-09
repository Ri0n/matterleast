#include <QtTest>

#include "notifications/DesktopPostFreshnessPolicy.h"

using namespace Mattermost;

class DesktopPostFreshnessPolicyTest final : public QObject
{
    Q_OBJECT
private slots:
    void historyIsNotANewDesktopNotification()
    {
        constexpr quint64 now = 1780000000000ULL;
        QVERIFY(!isFreshDesktopPost(now - 30ULL * 24 * 60 * 60 * 1000, now));
        QVERIFY(!isFreshDesktopPost(now - 24ULL * 60 * 60 * 1000, now));
        QVERIFY(!isFreshDesktopPost(now - 5ULL * 60 * 1000 - 1, now));
    }

    void freshAndSlightlyDelayedEvents()
    {
        constexpr quint64 now = 1780000000000ULL;
        QVERIFY(isFreshDesktopPost(now, now));
        QVERIFY(isFreshDesktopPost(now - 2ULL * 60 * 1000, now));
        QVERIFY(isFreshDesktopPost(now - 5ULL * 60 * 1000, now));
    }

    void malformedAndFutureDatedEvents()
    {
        constexpr quint64 now = 1780000000000ULL;
        QVERIFY(!isFreshDesktopPost(0, now));
        QVERIFY(!isFreshDesktopPost(now, 0));
        QVERIFY(isFreshDesktopPost(now + 60ULL * 1000, now));
        QVERIFY(!isFreshDesktopPost(now + 60ULL * 1000 + 1, now));
    }
};

QTEST_APPLESS_MAIN(DesktopPostFreshnessPolicyTest)
#include "DesktopPostFreshnessPolicyTest.moc"
