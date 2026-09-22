#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "chat-area/ChannelPostSource.h"

using namespace Mattermost;

namespace {

QString postId(int index)
{
    return QStringLiteral("post%1").arg(index, 22, 10, QLatin1Char('0'));
}

qint64 timestamp(int index)
{
    return 1000000000000LL + index * 1000LL;
}

QJsonObject rootPost(int index)
{
    return {
        {QStringLiteral("id"), postId(index)},
        {QStringLiteral("channel_id"), QStringLiteral("channel")},
        {QStringLiteral("user_id"), QStringLiteral("user")},
        {QStringLiteral("message"), QString::number(index)},
        {QStringLiteral("create_at"), timestamp(index)},
        {QStringLiteral("update_at"), timestamp(index)},
    };
}

QJsonObject channelJson(int rootCount, qint64 lastRootAt)
{
    return {
        {QStringLiteral("id"), QStringLiteral("channel")},
        {QStringLiteral("type"), QStringLiteral("D")},
        {QStringLiteral("name"), QStringLiteral("other-user")},
        {QStringLiteral("total_msg_count"), rootCount},
        {QStringLiteral("total_msg_count_root"), rootCount},
        {QStringLiteral("last_post_at"), lastRootAt},
        {QStringLiteral("last_root_post_at"), lastRootAt},
    };
}

} // namespace

class ChannelPostSourceLiveSyncTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void transientBackendEventExtendsSemanticTail()
    {
        Backend backend;
        BackendChannel channel(
            backend.getStorage(), channelJson(1, timestamp(1)));
        BackendPost* first = channel.addPost(rootPost(1));
        QVERIFY(first);

        ChannelPostSource source(backend, channel);
        QCOMPARE(source.itemCount(), 1);
        QCOMPARE(source.postIdAt(0), postId(1));
        QVERIFY(source.isAvailable(0));

        // Reproduce WebSocketEventHandler's cold-channel path: the post is
        // delivered globally but deliberately not retained in BackendChannel.
        BackendPost transient(rootPost(2), backend.getStorage());
        QVERIFY(!channel.postIdToPost.contains(transient.id));

        QSignalSpy countChanged(&source, &AbstractPostSource::itemCountChanged);
        emit backend.onNewPost(channel, transient);

        QCOMPARE(countChanged.size(), 1);
        QCOMPARE(source.itemCount(), 2);
        QCOMPARE(source.indexOfPost(postId(2)), 1);
        QCOMPARE(source.postIdAt(1), postId(2));
        QVERIFY(!source.isAvailable(1));

        // A later normal server page rehydrates the body. The semantic row
        // already exists, so it becomes available in place rather than appearing
        // only after source/process reconstruction.
        QSignalSpy bodyChanged(
            &source, &AbstractPostSource::bodyAvailabilityChanged);
        channel.mergePostContext(
            QJsonArray {postId(2)},
            QJsonObject {{postId(2), rootPost(2)}});

        QVERIFY(source.isAvailable(1));
        QVERIFY(source.postAt(1));
        QCOMPARE(source.postAt(1)->id, postId(2));
        QVERIFY(!bodyChanged.isEmpty());
    }

    void residentBackendEventIsNotAppendedTwice()
    {
        Backend backend;
        BackendChannel channel(
            backend.getStorage(), channelJson(1, timestamp(1)));
        QVERIFY(channel.addPost(rootPost(1)));

        ChannelPostSource source(backend, channel);
        QCOMPARE(source.itemCount(), 1);

        BackendPost* second = channel.addPost(rootPost(2));
        QVERIFY(second);

        emit channel.onNewPost(*second);
        QCOMPARE(source.itemCount(), 2);
        QCOMPARE(source.indexOfPost(second->id), 1);

        // WebSocketEventHandler emits the global notification after the
        // channel-local one for resident posts.
        emit backend.onNewPost(channel, *second);
        QCOMPARE(source.itemCount(), 2);
        QCOMPARE(source.indexOfPost(second->id), 1);
    }
};

QTEST_MAIN(ChannelPostSourceLiveSyncTest)

#include "ChannelPostSourceLiveSyncTest.moc"
