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

        QCOMPARE(model.append(entry), 0);
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

        model.append(first);
        model.append(second);

        QCOMPARE(model.count(), 2);
        QVERIFY(model.shouldShowTabBar());
    }

    void repeatedChannelDestinationIsIdempotent()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry channel;
        channel.channelId = QStringLiteral("channel-a");
        channel.postId = QStringLiteral("post-1");

        QCOMPARE(model.append(channel), 0);

        channel.postId = QStringLiteral("post-2");
        QCOMPARE(model.append(channel), 0);
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.at(0)->postId, QStringLiteral("post-2"));
    }

    void replaceCannotCreateDuplicateDestination()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry first;
        first.channelId = QStringLiteral("channel-a");
        NavigationTabsModel::Entry second;
        second.channelId = QStringLiteral("channel-b");

        model.append(first);
        model.append(second);

        second.channelId = QStringLiteral("channel-a");
        QVERIFY(!model.replace(1, second));
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.at(1)->channelId, QStringLiteral("channel-b"));
    }

    void threadDestinationIsDeduplicatedAndBookmarkUpdated()
    {
        NavigationTabsModel model;
        NavigationTabsModel::Entry thread;
        thread.channelId = QStringLiteral("channel-a");
        thread.rootId = QStringLiteral("root-a");
        thread.postId = QStringLiteral("reply-1");

        QCOMPARE(model.append(thread), 0);

        thread.postId = QStringLiteral("reply-2");
        QCOMPARE(model.append(thread), 0);
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
            model.append(entry);
        }

        model.move(0, 2);

        QCOMPARE(model.at(0)->channelId, QStringLiteral("b"));
        QCOMPARE(model.at(1)->channelId, QStringLiteral("c"));
        QCOMPARE(model.at(2)->channelId, QStringLiteral("a"));
    }
};

QTEST_APPLESS_MAIN(NavigationTabsModelTest)

#include "NavigationTabsModelTest.moc"
