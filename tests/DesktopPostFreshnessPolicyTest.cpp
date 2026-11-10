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

    void channelRootsRespectServerReadCursor()
    {
        QVERIFY(!isUnreadDesktopPost(1500, false, true, 1500, 0));
        QVERIFY(!isUnreadDesktopPost(1499, false, false, 1500, 0));
        QVERIFY(isUnreadDesktopPost(1501, false, true, 1500, 0));
        // No authoritative snapshot yet: a live message is still eligible.
        QVERIFY(isUnreadDesktopPost(1500, false, true, 0, 0));
    }

    void crtRepliesUseTheirOwnServerReadCursor()
    {
        // Reading the parent channel does not read a followed CRT thread.
        QVERIFY(isUnreadDesktopPost(1500, true, true, 2000, 1400));
        QVERIFY(isUnreadDesktopPost(1500, true, true, 2000, 0));
        QVERIFY(!isUnreadDesktopPost(1500, true, true, 2000, 1500));
        QVERIFY(!isUnreadDesktopPost(1500, true, true, 2000, 1600));
        // In legacy mode, the parent channel also provides a read boundary.
        QVERIFY(!isUnreadDesktopPost(1500, true, false, 1500, 0));
        QVERIFY(isUnreadDesktopPost(1501, true, false, 1500, 1400));
    }
};

QTEST_APPLESS_MAIN(DesktopPostFreshnessPolicyTest)
#include "DesktopPostFreshnessPolicyTest.moc"
