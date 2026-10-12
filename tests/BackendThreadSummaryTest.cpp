#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

using namespace Mattermost;

namespace {
QString replyId(int n) { return QStringLiteral("reply-%1").arg(n); }
qint64 replyTime(int n) { return 1000000000000LL + qint64(n) * 1000; }

QJsonObject rootSnapshot(int count, int newest)
{
    return {
        {"id", "root"}, {"channel_id", "channel"},
        {"user_id", "user"}, {"message", "root"},
        {"create_at", replyTime(0)}, {"update_at", replyTime(0)},
        {"reply_count", count}, {"last_reply_at", replyTime(newest)}
    };
}

QJsonObject reply(int n)
{
    return {
        {"id", replyId(n)}, {"channel_id", "channel"},
        {"root_id", "root"}, {"user_id", "user"},
        {"message", QString::number(n)},
        {"create_at", replyTime(n)}, {"update_at", replyTime(n)}
    };
}
} // namespace

class BackendThreadSummaryTest : public QObject
{
    Q_OBJECT
private slots:
    void delayedWebSocketReplyAlreadyCountedByRestSnapshot()
    {
        Backend backend;
        BackendChannel channel(backend.getStorage(),
                               QJsonObject {{"id", "channel"}, {"type", "O"}});
        BackendPost* root = channel.addPost(rootSnapshot(3, 3));
        QVERIFY(root);
        QCOMPARE(root->reply_count, int64_t(3));

        // REST loaded the root's reply_count but not reply bodies. The event
        // for reply 3 is new to our resident cache, not new to the server.
        BackendPost* late = channel.addPost(reply(3));
        QVERIFY(late);
        QCOMPARE(root->reply_count, int64_t(3));
        QCOMPARE(root->last_reply_at, uint64_t(replyTime(3)));

        // In contrast, reply 4 was not represented in that REST snapshot.
        QVERIFY(channel.addPost(reply(4)));
        QCOMPARE(root->reply_count, int64_t(4));
    }

    void reorderedWebSocketEventsAfterSnapshotAreEachCounted()
    {
        Backend backend;
        BackendChannel channel(backend.getStorage(),
                               QJsonObject {{"id", "channel"}, {"type", "O"}});
        BackendPost* root = channel.addPost(rootSnapshot(1, 1));
        QVERIFY(root);

        QVERIFY(channel.addPost(reply(3)));
        QCOMPARE(root->reply_count, int64_t(2));
        QVERIFY(channel.addPost(reply(2)));
        // Comparing with mutable last_reply_at would incorrectly skip #2.
        QCOMPARE(root->reply_count, int64_t(3));
        QCOMPARE(root->last_reply_at, uint64_t(replyTime(3)));

        // Idempotent identity admission must not count the same event twice.
        QVERIFY(channel.addPost(reply(3)));
        QCOMPARE(root->reply_count, int64_t(3));
    }

    void refreshedServerSnapshotResetsReplyBoundary()
    {
        Backend backend;
        BackendChannel channel(backend.getStorage(),
                               QJsonObject {{"id", "channel"}, {"type", "O"}});
        BackendPost* root = channel.addPost(rootSnapshot(1, 1));
        QVERIFY(root);
        QVERIFY(channel.addPost(reply(2)));
        QCOMPARE(root->reply_count, int64_t(2));

        // A server refresh now includes #2 and #3. Even though #3 was
        // not resident, subsequent delivery must not add a phantom row.
        const QJsonObject snapshot = rootSnapshot(3, 3);
        channel.mergePostContext(QJsonArray {QStringLiteral("root")},
                                 QJsonObject {{QStringLiteral("root"), snapshot}});
        QCOMPARE(root->reply_count, int64_t(3));
        QVERIFY(channel.addPost(reply(3)));
        QCOMPARE(root->reply_count, int64_t(3));
        QVERIFY(channel.addPost(reply(4)));
        QCOMPARE(root->reply_count, int64_t(4));
    }

    void missingSnapshotBoundaryKeepsConservativeLocalCounting()
    {
        Backend backend;
        BackendChannel channel(backend.getStorage(),
                               QJsonObject {{"id", "channel"}, {"type", "O"}});
        QJsonObject snapshot = rootSnapshot(2, 2);
        snapshot.remove("last_reply_at");
        BackendPost* root = channel.addPost(snapshot);
        QVERIFY(root);
        QVERIFY(channel.addPost(reply(1)));
        // A partial payload does not prove whether that reply was counted.
        QCOMPARE(root->reply_count, int64_t(3));
    }
};

QTEST_MAIN(BackendThreadSummaryTest)
#include "BackendThreadSummaryTest.moc"
