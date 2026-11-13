#include <QtTest>

#include "navigation/ThreadSidebarContextPolicy.h"

using namespace Mattermost;

class ThreadSidebarContextPolicyTest final : public QObject
{
    Q_OBJECT

private slots:
    void disabledDoesNotChangeNavigation()
    {
        QVERIFY(!shouldOpenThreadParentChannel(false, true, QStringLiteral("channel-a"), QStringLiteral("channel-b")));
        QVERIFY(!shouldCloseThreadDockForChannel(false, QStringLiteral("channel-a"), QStringLiteral("channel-b")));

        NavigationTabsModel model;
        NavigationTabsModel::Entry channel;
        channel.channelId = QStringLiteral("channel-a");
        model.append(channel);
        QCOMPARE(chooseThreadParentTab(false, model, 0,
                                      QStringLiteral("channel-b"),
                                      QStringLiteral("thread-b")).route,
                 ThreadParentTabRoute::KeepCurrent);
    }

    void switchesParentForChannelsAndDirectConversations()
    {
        QVERIFY(shouldOpenThreadParentChannel(true, true, QStringLiteral("channel-a"), QStringLiteral("channel-b")));
        QVERIFY(shouldOpenThreadParentChannel(true, true, QStringLiteral("dm-a"), QStringLiteral("gm-b")));
        QVERIFY(!shouldOpenThreadParentChannel(true, true, QStringLiteral("channel-a"), QStringLiteral("channel-a")));
        // Saved/Search collection is a central surface without a channel.
        QVERIFY(shouldOpenThreadParentChannel(true, true, QString(), QStringLiteral("dm-a")));
        QVERIFY(!shouldOpenThreadParentChannel(true, false, QStringLiteral("channel-a"), QStringLiteral("channel-b")));
    }

    void pinnedChannelNeverReceivesForeignThread()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry pinned;
        pinned.channelId = QStringLiteral("channel-a");
        pinned.pinned = true;
        QCOMPARE(model.append(pinned), 0);

        const auto missing = chooseThreadParentTab(
            true, model, 0, QStringLiteral("channel-b"), QStringLiteral("thread-b"));
        QCOMPARE(missing.route, ThreadParentTabRoute::CreateNew);
        QCOMPARE(model.count(), 1);
        QVERIFY(model.at(0)->pinned);
        QCOMPARE(model.at(0)->channelId, QStringLiteral("channel-a"));

        NavigationTabsModel::Entry parent;
        parent.channelId = QStringLiteral("channel-b");
        QCOMPARE(model.append(parent), 1);
        const auto existing = chooseThreadParentTab(
            true, model, 0, QStringLiteral("channel-b"), QStringLiteral("thread-b"));
        QCOMPARE(existing.route, ThreadParentTabRoute::ActivateExisting);
        QCOMPARE(existing.index, 1);
        QCOMPARE(chooseThreadParentTab(true, model, 1,
                                      QStringLiteral("channel-b"),
                                      QStringLiteral("thread-b")).route,
                 ThreadParentTabRoute::KeepCurrent);
    }

    void independentThreadTabsRemainIndependent()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry threadOnly;
        threadOnly.channelId = QStringLiteral("channel-a");
        threadOnly.rootId = QStringLiteral("root-a");
        QCOMPARE(model.append(threadOnly), 0);

        // Explicit open-in-tab with a matching root cannot create a channel tab.
        QCOMPARE(chooseThreadParentTab(true, model, 0,
                                      QStringLiteral("channel-a"),
                                      QStringLiteral("root-a")).route,
                 ThreadParentTabRoute::KeepCurrent);
        // Normal navigation to another thread must not add a dock to an
        // unrelated thread-only tab.
        QCOMPARE(chooseThreadParentTab(true, model, 0,
                                      QStringLiteral("channel-b"),
                                      QStringLiteral("root-b")).route,
                 ThreadParentTabRoute::CreateNew);
        QCOMPARE(chooseThreadParentTab(true, model, 0,
                                      QStringLiteral("channel-a"),
                                      QStringLiteral("other-root")).route,
                 ThreadParentTabRoute::CreateNew);
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
