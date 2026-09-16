#include <QtTest>

#include <QJsonObject>

#include "backend/Backend.h"
#include "backend/FollowingModel.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"

using namespace Mattermost;

namespace {

QJsonObject postJson(const QString& id,
                     quint64 createAt,
                     const QString& rootId = QString())
{
    return QJsonObject {
        {QStringLiteral("id"), id},
        {QStringLiteral("channel_id"), QStringLiteral("channel")},
        {QStringLiteral("root_id"), rootId},
        {QStringLiteral("user_id"), QStringLiteral("user")},
        {QStringLiteral("message"), id},
        {QStringLiteral("create_at"), static_cast<double>(createAt)},
        {QStringLiteral("update_at"), static_cast<double>(createAt)},
    };
}

BackendChannel* makeChannel(Backend& backend)
{
    Storage& storage = backend.getStorage();
    storage.addUser(QJsonObject {
        {QStringLiteral("id"), QStringLiteral("me")},
        {QStringLiteral("username"), QStringLiteral("me")},
    }, true);
    storage.addUser(QJsonObject {
        {QStringLiteral("id"), QStringLiteral("user")},
        {QStringLiteral("username"), QStringLiteral("user")},
    });

    BackendTeam* team = storage.addTeam(QJsonObject {
        {QStringLiteral("id"), QStringLiteral("team")},
        {QStringLiteral("name"), QStringLiteral("team")},
        {QStringLiteral("display_name"), QStringLiteral("Team")},
    });
    if (!team) {
        return nullptr;
    }

    return storage.addTeamChannel(*team, QJsonObject {
        {QStringLiteral("id"), QStringLiteral("channel")},
        {QStringLiteral("team_id"), QStringLiteral("team")},
        {QStringLiteral("type"), QStringLiteral("O")},
        {QStringLiteral("name"), QStringLiteral("channel")},
        {QStringLiteral("display_name"), QStringLiteral("Channel")},
        {QStringLiteral("last_post_at"), 400.0},
    });
}

BackendChannel* makeConversationChannel(Backend& backend, const QString& type)
{
    Storage& storage = backend.getStorage();
    storage.addUser(QJsonObject {
        {QStringLiteral("id"), QStringLiteral("me")},
        {QStringLiteral("username"), QStringLiteral("me")},
    }, true);
    storage.addUser(QJsonObject {
        {QStringLiteral("id"), QStringLiteral("user")},
        {QStringLiteral("username"), QStringLiteral("user")},
    });

    QJsonObject json {
        {QStringLiteral("id"), QStringLiteral("channel")},
        {QStringLiteral("type"), type},
        {QStringLiteral("name"), type == QStringLiteral("D")
            ? QStringLiteral("me__user") : QStringLiteral("group")},
        {QStringLiteral("display_name"), QStringLiteral("me, user")},
        {QStringLiteral("last_post_at"), 200.0},
        {QStringLiteral("last_root_post_at"), 100.0},
        {QStringLiteral("total_msg_count"), 2},
        {QStringLiteral("total_msg_count_root"), 1},
    };

    return type == QStringLiteral("D")
        ? storage.addDirectChannel(json)
        : storage.addGroupChannel(json);
}

} // namespace

class ManualUnreadReadStateTest : public QObject
{
    Q_OBJECT

private slots:
    void channelManualUnreadIsConsumedByReadThroughAtEnd()
    {
        Backend backend;
        BackendChannel* channel = makeChannel(backend);
        QVERIFY(channel);

        BackendPost* first = channel->addPost(postJson(QStringLiteral("post-1"), 100));
        BackendPost* second = channel->addPost(postJson(QStringLiteral("post-2"), 200));
        BackendPost* last = channel->addPost(postJson(QStringLiteral("post-3"), 300));
        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(last);

        FollowingModel& model = FollowingModel::instance(backend);
        model.markPostUnread(channel->id, QString(), first->id, first->create_at);

        const FollowingModel::Entry* marked = model.findEntry(channel->id);
        QVERIFY(marked);
        QCOMPARE(marked->resumeState, FollowingModel::ResumeState::FirstUnread);
        QCOMPARE(marked->firstUnreadPostId, first->id);
        QVERIFY(marked->requiresAttention());

        // ChatLogWidget releases its manual-unread visibility gate with the
        // newest lower edge actually observed while the marker was away. Once
        // that high-water reaches the channel end, the local manual projection
        // must disappear rather than pinning Attention on the marked post.
        model.observeReadThrough(channel->id, QString(), *last, true);
        QVERIFY(!model.findEntry(channel->id));
    }

    void threadManualUnreadAdvancesPastTheMarker()
    {
        Backend backend;
        BackendChannel* channel = makeChannel(backend);
        QVERIFY(channel);

        BackendPost* root = channel->addPost(postJson(QStringLiteral("root"), 100));
        BackendPost* firstReply = channel->addPost(
            postJson(QStringLiteral("reply-1"), 200, root->id));
        BackendPost* secondReply = channel->addPost(
            postJson(QStringLiteral("reply-2"), 300, root->id));
        QVERIFY(root);
        QVERIFY(firstReply);
        QVERIFY(secondReply);

        FollowingModel& model = FollowingModel::instance(backend);
        model.markPostUnread(channel->id, root->id,
                             firstReply->id, firstReply->create_at);

        model.observeReadThrough(channel->id, root->id, *firstReply, false);
        const FollowingModel::Entry* afterMarker = model.findEntry(channel->id, root->id);
        QVERIFY(afterMarker);
        QCOMPARE(afterMarker->readThroughPostId, firstReply->id);
        QCOMPARE(afterMarker->resumeState, FollowingModel::ResumeState::FirstUnread);
        QCOMPARE(afterMarker->firstUnreadPostId, secondReply->id);

        model.observeReadThrough(channel->id, root->id, *secondReply, true);
        const FollowingModel::Entry* atEnd = model.findEntry(channel->id, root->id);
        QVERIFY(atEnd);
        QCOMPARE(atEnd->readThroughPostId, secondReply->id);
        QCOMPARE(atEnd->resumeState, FollowingModel::ResumeState::AtEnd);
        QVERIFY(atEnd->firstUnreadPostId.isEmpty());
    }
    void dmGmRootReadLeavesHiddenThreadUnread_data()
    {
        QTest::addColumn<QString>("channelType");
        QTest::newRow("direct") << QStringLiteral("D");
        QTest::newRow("group") << QStringLiteral("G");
    }

    void dmGmRootReadLeavesHiddenThreadUnread()
    {
        QFETCH(QString, channelType);

        Backend backend;
        BackendChannel* channel = makeConversationChannel(backend, channelType);
        QVERIFY(channel);
        QCOMPARE(channel->last_root_post_at, uint64_t(100));
        QCOMPARE(channel->last_post_at, uint64_t(200));

        BackendPost* root = channel->addPost(postJson(QStringLiteral("root"), 100));
        BackendPost* reply = channel->addPost(
            postJson(QStringLiteral("reply"), 200, root->id));
        QVERIFY(root);
        QVERIFY(reply);

        FollowingModel& model = FollowingModel::instance(backend);
        model.markPostUnread(channel->id, QString(), root->id, root->create_at);
        model.markPostUnread(channel->id, root->id, reply->id, reply->create_at);

        const FollowingModel::Entry* parentBefore = model.findEntry(channel->id);
        const FollowingModel::Entry* threadBefore = model.findEntry(channel->id, root->id);
        QVERIFY(parentBefore);
        QVERIFY(parentBefore->requiresAttention());
        QVERIFY(threadBefore);
        QVERIFY(threadBefore->requiresAttention());

        // This is the DM/GM regression: the root source is fully consumed
        // at last_root_post_at, while a newer reply exists only in the
        // collapsed thread. Parent Attention must disappear, but the
        // thread's unread state must remain intact.
        model.observeReadThrough(channel->id, QString(), *root, true, true);

        QVERIFY(!model.findEntry(channel->id));
        const FollowingModel::Entry* threadAfter =
            model.findEntry(channel->id, root->id);
        QVERIFY(threadAfter);
        QVERIFY(threadAfter->requiresAttention());
        QCOMPARE(threadAfter->resumeState, FollowingModel::ResumeState::FirstUnread);
        QCOMPARE(threadAfter->firstUnreadPostId, reply->id);
    }

};

QTEST_MAIN(ManualUnreadReadStateTest)
#include "ManualUnreadReadStateTest.moc"
