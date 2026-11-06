#include <QtTest>

#include "navigation/ThreadSidebarContextPolicy.h"

using namespace Mattermost;

class ThreadSidebarContextPolicyTest final : public QObject
{
    Q_OBJECT

private slots:
    void disabledDoesNotChangeNavigation()
    {
        QVERIFY(!shouldOpenThreadParentChannel(false, false, true, QStringLiteral("channel-a"), QStringLiteral("channel-b")));
        QVERIFY(!shouldCloseThreadDockForChannel(false, QStringLiteral("channel-a"), QStringLiteral("channel-b")));
    }

    void switchesParentForUnpinnedChannelAndDirectConversations()
    {
        QVERIFY(shouldOpenThreadParentChannel(true, false, true, QStringLiteral("channel-a"), QStringLiteral("channel-b")));
        QVERIFY(shouldOpenThreadParentChannel(true, false, true, QStringLiteral("dm-a"), QStringLiteral("gm-b")));
        QVERIFY(!shouldOpenThreadParentChannel(true, false, true, QStringLiteral("channel-a"), QStringLiteral("channel-a")));
        // Saved/Search collection is a central surface without a channel.
        QVERIFY(shouldOpenThreadParentChannel(true, false, true, QString(), QStringLiteral("dm-a")));
    }

    void pinnedAndIndependentTabsKeepTheirCentralDestination()
    {
        QVERIFY(!shouldOpenThreadParentChannel(true, true, true, QStringLiteral("channel-a"), QStringLiteral("channel-b")));
        QVERIFY(!shouldOpenThreadParentChannel(true, false, false, QStringLiteral("channel-a"), QStringLiteral("channel-b")));
    }

    void unrelatedDockClosesOnlyWhenEnabled()
    {
        QVERIFY(shouldCloseThreadDockForChannel(true, QStringLiteral("dm-a"), QStringLiteral("channel-b")));
        QVERIFY(shouldCloseThreadDockForChannel(true, QStringLiteral("gm-a"), QStringLiteral("dm-b")));
        QVERIFY(!shouldCloseThreadDockForChannel(true, QStringLiteral("dm-a"), QStringLiteral("dm-a")));
        QVERIFY(!shouldCloseThreadDockForChannel(true, QString(), QStringLiteral("dm-a")));
    }
};

QTEST_GUILESS_MAIN(ThreadSidebarContextPolicyTest)
#include "ThreadSidebarContextPolicyTest.moc"
