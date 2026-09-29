#include <QtTest>

#include "navigation/NavigationTabsModel.h"

using Mattermost::NavigationTabsModel;

class NavigationTabsModelTest : public QObject
{
    Q_OBJECT

private slots:
    void singleDestinationKeepsTabBarHidden()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry entry;
        entry.channelId = QStringLiteral("channel-a");
        entry.title = QStringLiteral("A");

        QCOMPARE(model.append(entry, false), 0);
        QCOMPARE(model.count(), 1);
        QVERIFY(!model.shouldShowTabBar());
    }

    void secondDestinationShowsTabBar()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry first;
        first.channelId = QStringLiteral("channel-a");
        NavigationTabsModel::Entry second;
        second.channelId = QStringLiteral("channel-b");

        model.append(first, false);
        model.append(second, false);

        QCOMPARE(model.count(), 2);
        QVERIFY(model.shouldShowTabBar());
    }

    void explicitChannelTabsMayDuplicateDestination()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry channel;
        channel.channelId = QStringLiteral("channel-a");

        QCOMPARE(model.append(channel, false), 0);
        QCOMPARE(model.append(channel, false), 1);
        QCOMPARE(model.count(), 2);
    }

    void threadDestinationIsDeduplicatedAndBookmarkUpdated()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry thread;
        thread.channelId = QStringLiteral("channel-a");
        thread.rootId = QStringLiteral("root-a");
        thread.postId = QStringLiteral("reply-1");

        QCOMPARE(model.append(thread, true), 0);

        thread.postId = QStringLiteral("reply-2");
        QCOMPARE(model.append(thread, true), 0);
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.at(0)->postId, QStringLiteral("reply-2"));
    }

    void movingTabsKeepsSemanticTargetsAligned()
    {
        NavigationTabsModel model;
        for (const QString& id : {
                 QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") }) {
            NavigationTabsModel::Entry entry;
            entry.channelId = id;
            model.append(entry, false);
        }

        model.move(0, 2);

        QCOMPARE(model.at(0)->channelId, QStringLiteral("b"));
        QCOMPARE(model.at(1)->channelId, QStringLiteral("c"));
        QCOMPARE(model.at(2)->channelId, QStringLiteral("a"));
    }
};

QTEST_APPLESS_MAIN(NavigationTabsModelTest)

#include "NavigationTabsModelTest.moc"
