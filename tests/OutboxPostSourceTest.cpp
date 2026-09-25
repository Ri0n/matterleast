#include <memory>
#include <vector>

#include <QDir>
#include <QJsonObject>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QtTest>

#include "backend/Backend.h"
#include "backend/PendingPostService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "chat-area/OutboxPostSource.h"

using namespace Mattermost;

namespace {

QJsonObject postJson(const QString& id, qint64 createAt)
{
    return QJsonObject {
        {QStringLiteral("id"), id},
        {QStringLiteral("channel_id"), QStringLiteral("channel")},
        {QStringLiteral("user_id"), QStringLiteral("other")},
        {QStringLiteral("message"), id},
        {QStringLiteral("create_at"), QJsonValue::fromVariant(createAt)},
        {QStringLiteral("update_at"), QJsonValue::fromVariant(createAt)},
    };
}

class FakePostSource final : public AbstractPostSource
{
public:
    explicit FakePostSource(Storage& storage)
        : storage_(storage)
    {
    }

    int itemCount() const override
    {
        return static_cast<int>(posts_.size());
    }

    bool isAvailable(int index) const override
    {
        return index >= 0 && index < itemCount() && posts_[index] != nullptr;
    }

    BackendPost* postAt(int index) const override
    {
        return isAvailable(index) ? posts_[index].get() : nullptr;
    }

    QString postIdAt(int index) const override
    {
        BackendPost* post = postAt(index);
        return post ? post->id : QString();
    }

    int indexOfPost(const QString& id) const override
    {
        for (int i = 0; i < itemCount(); ++i) {
            if (posts_[i] && posts_[i]->id == id) {
                return i;
            }
        }
        return -1;
    }

    void requestRange(int first,
                      int last,
                      RequestReason,
                      quint64) override
    {
        if (first <= last) {
            emit rangeAvailable(first, last);
        }
        emit rangeRequestFinished(first, last);
    }

    void appendCoarse(const QString& id,
                      qint64 createAt,
                      const QString& pendingPostId = {})
    {
        QJsonObject raw = postJson(id, createAt);
        if (!pendingPostId.isEmpty()) {
            raw.insert(QStringLiteral("pending_post_id"), pendingPostId);
        }
        posts_.push_back(
            std::make_unique<BackendPost>(raw, storage_));
        emit itemCountChanged(itemCount());
        emit rangeAvailable(itemCount() - 1, itemCount() - 1);
    }

private:
    Storage& storage_;
    std::vector<std::unique_ptr<BackendPost>> posts_;
};

QJsonObject channelJson()
{
    return QJsonObject {
        {QStringLiteral("id"), QStringLiteral("channel")},
        {QStringLiteral("type"), QStringLiteral("O")},
        {QStringLiteral("display_name"), QStringLiteral("Channel")},
    };
}

} // namespace

class OutboxPostSourceTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
             + QStringLiteral("/outbox")).removeRecursively();
    }

    void authoritativeRowsInsertBeforePendingTail()
    {
        Backend backend;
        backend.getStorage().addUser(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("me")},
                {QStringLiteral("username"), QStringLiteral("me")},
            },
            true);
        BackendChannel channel(backend.getStorage(), channelJson());

        FakePostSource raw(backend.getStorage());
        raw.appendCoarse(QStringLiteral("server-a"), 1000);

        PendingPostService& service = PendingPostService::instance(backend);
        OutboxPostSource source(
            raw, service, channel.id, QString(), &backend);

        QSignalSpy inserted(&source, &AbstractPostSource::itemsInserted);
        const QString pendingId = service.enqueue(
            channel,
            QStringLiteral("optimistic"),
            {},
            {},
            {},
            QStringLiteral("pending-1"));

        QCOMPARE(pendingId, QStringLiteral("pending-1"));
        QCOMPARE(source.itemCount(), 2);
        QCOMPARE(source.authoritativeItemCount(), 1);
        QVERIFY(source.isAuthoritativeRow(0));
        QVERIFY(!source.isAuthoritativeRow(1));
        QVERIFY(source.isPostPositionAuthoritative(QStringLiteral("server-a")));
        QCOMPARE(source.postIdAt(0), QStringLiteral("server-a"));
        QCOMPARE(source.postIdAt(1), pendingId);
        QVERIFY(source.isPendingIndex(1));
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(inserted.at(0).at(0).toInt(), 1);
        QCOMPARE(inserted.at(0).at(1).toInt(), 1);

        inserted.clear();
        raw.appendCoarse(QStringLiteral("server-b"), 2000);

        QCOMPARE(source.itemCount(), 3);
        QCOMPARE(source.postIdAt(0), QStringLiteral("server-a"));
        QCOMPARE(source.postIdAt(1), QStringLiteral("server-b"));
        QCOMPARE(source.postIdAt(2), pendingId);
        QVERIFY(source.isPendingIndex(2));

        // A coarse authoritative tail resize must become an exact insertion
        // immediately before the additive outbox tail.
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(inserted.at(0).at(0).toInt(), 1);
        QCOMPARE(inserted.at(0).at(1).toInt(), 1);

        QVERIFY(service.cancel(pendingId));
        QCOMPARE(source.itemCount(), 2);
        QCOMPARE(source.indexOfPost(pendingId), -1);
    }

    void confirmationIsOneReplacementNotInsertRemoveOrLayout()
    {
        Backend backend;
        backend.getStorage().addUser(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("me-promote")},
                {QStringLiteral("username"), QStringLiteral("me-promote")},
            },
            true);
        BackendChannel channel(backend.getStorage(), channelJson());

        FakePostSource raw(backend.getStorage());
        raw.appendCoarse(QStringLiteral("server-a"), 1000);

        PendingPostService& service = PendingPostService::instance(backend);
        OutboxPostSource source(
            raw, service, channel.id, QString(), &backend);

        const QString pendingId = service.enqueue(
            channel, QStringLiteral("optimistic"), {}, {}, {},
            QStringLiteral("pending-promote"));
        QVERIFY(!pendingId.isEmpty());

        QSignalSpy inserted(&source, &AbstractPostSource::itemsInserted);
        QSignalSpy removed(&source, &AbstractPostSource::itemsRemoved);
        QSignalSpy layouts(&source, &AbstractPostSource::layoutChanged);
        QSignalSpy promoted(&source, &OutboxPostSource::pendingPromoted);

        raw.appendCoarse(
            QStringLiteral("server-promote"), 2000, pendingId);
        QCOMPARE(inserted.count(), 0);

        BackendPost* authoritative = raw.postAt(raw.itemCount() - 1);
        QVERIFY(authoritative);
        emit backend.onNewPost(channel, *authoritative);

        QCOMPARE(promoted.count(), 1);
        QCOMPARE(promoted.at(0).at(0).toInt(), 1);
        QCOMPARE(promoted.at(0).at(1).toString(), pendingId);
        QCOMPARE(promoted.at(0).at(2).toString(),
                 QStringLiteral("server-promote"));
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(layouts.count(), 0);
        QCOMPARE(source.itemCount(), 2);
        QCOMPARE(source.postIdAt(1), QStringLiteral("server-promote"));

        QCoreApplication::processEvents();
        QCOMPARE(inserted.count(), 0);
        QCOMPARE(removed.count(), 0);
        QCOMPARE(layouts.count(), 0);
    }

    void stopsAutomaticDeliveryAfterThreeAuthoritativeMessages()
    {
        Backend backend;
        backend.getStorage().addUser(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("me3")},
                {QStringLiteral("username"), QStringLiteral("me3")},
            },
            true);
        BackendChannel channel(backend.getStorage(), channelJson());

        PendingPostService& service = PendingPostService::instance(backend);
        service.setVisibilityPredicate(
            channel.id,
            QString(),
            [](const BackendPost& post) {
                return post.type != QStringLiteral("system_join_leave");
            });
        const QString pendingId = service.enqueue(
            channel, QStringLiteral("pending"), {}, {}, {},
            QStringLiteral("pending-cutoff"));
        QVERIFY(!pendingId.isEmpty());

        // Hidden join/part churn is outside the user's presented conversation
        // and must not consume the "three messages passed us" retry budget.
        QJsonObject hiddenJson = postJson(QStringLiteral("membership"), 999);
        hiddenJson.insert(QStringLiteral("type"), QStringLiteral("system_join_leave"));
        hiddenJson.insert(
            QStringLiteral("props"),
            QJsonObject {{QStringLiteral("username"), QStringLiteral("someone-else")}});
        BackendPost hidden(hiddenJson, backend.getStorage());
        emit backend.onNewPost(channel, hidden);
        const PendingPost* afterHidden = service.pendingPost(pendingId);
        QVERIFY(afterHidden);
        QCOMPARE(afterHidden->interveningPostCount, 0);

        for (int i = 0; i < PendingPostService::MaxInterveningPosts; ++i) {
            BackendPost remote(
                postJson(QStringLiteral("remote-%1").arg(i), 1000 + i),
                backend.getStorage());
            emit backend.onNewPost(channel, remote);
        }

        const PendingPost* pending = service.pendingPost(pendingId);
        QVERIFY(pending);
        QCOMPARE(pending->interveningPostCount,
                 PendingPostService::MaxInterveningPosts);
        QVERIFY(pending->state == PendingPostState::Failed);
        QVERIFY(pending->failureText.contains(QStringLiteral("Three")));

        QVERIFY(service.cancel(pendingId));
    }

    void confirmedOwnPostEchoDoesNotAdvanceNextPending()
    {
        Backend backend;
        backend.getStorage().addUser(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("me4")},
                {QStringLiteral("username"), QStringLiteral("me4")},
            },
            true);
        BackendChannel channel(backend.getStorage(), channelJson());

        PendingPostService& service = PendingPostService::instance(backend);
        const QString first = service.enqueue(
            channel, QStringLiteral("first"), {}, {}, {},
            QStringLiteral("pending-first"));
        const QString second = service.enqueue(
            channel, QStringLiteral("second"), {}, {}, {},
            QStringLiteral("pending-second"));
        QVERIFY(!first.isEmpty());
        QVERIFY(!second.isEmpty());

        QJsonObject confirmedJson =
            postJson(QStringLiteral("server-first"), 2000);
        confirmedJson.insert(
            QStringLiteral("pending_post_id"), first);
        confirmedJson.insert(
            QStringLiteral("user_id"), QStringLiteral("me4"));
        BackendPost confirmed(confirmedJson, backend.getStorage());

        // First delivery confirms/removes the FIFO head.
        emit backend.onNewPost(channel, confirmed);
        QVERIFY(!service.pendingPost(first));
        const PendingPost* next = service.pendingPost(second);
        QVERIFY(next);
        QCOMPARE(next->interveningPostCount, 0);

        // The same server post can be delivered again when HTTP ingestion wins
        // the race with the websocket echo. It must remain acknowledgement of
        // the previous local item, not consume the next item's retry budget.
        emit backend.onNewPost(channel, confirmed);
        next = service.pendingPost(second);
        QVERIFY(next);
        QCOMPARE(next->interveningPostCount, 0);

        QVERIFY(service.cancel(second));
    }

    void acceptsPreuploadedAttachmentOptimisticEntries()
    {
        Backend backend;
        backend.getStorage().addUser(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("me5")},
                {QStringLiteral("username"), QStringLiteral("me5")},
            },
            true);
        BackendChannel channel(backend.getStorage(), channelJson());

        PendingAttachment attachment;
        attachment.path = QStringLiteral("/tmp/example.png");
        attachment.fileId = QStringLiteral("file-id");

        PendingPostService& service = PendingPostService::instance(backend);
        const QString pendingId = service.enqueue(
            channel,
            QStringLiteral("text with upload"),
            {attachment},
            {},
            {},
            QStringLiteral("pending-attachment"));

        QCOMPARE(pendingId, QStringLiteral("pending-attachment"));
        const PendingPost* pending = service.pendingPost(pendingId);
        QVERIFY(pending);
        QCOMPARE(pending->attachments.size(), 1);
        QCOMPARE(pending->attachments.first().path,
                 QStringLiteral("/tmp/example.png"));
        QCOMPARE(pending->attachments.first().fileId,
                 QStringLiteral("file-id"));

        QVERIFY(service.cancel(pendingId));
    }

    void multiplePendingRowsKeepFifoPresentationOrder()
    {
        Backend backend;
        backend.getStorage().addUser(
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("me2")},
                {QStringLiteral("username"), QStringLiteral("me2")},
            },
            true);
        BackendChannel channel(backend.getStorage(), channelJson());

        FakePostSource raw(backend.getStorage());
        PendingPostService& service = PendingPostService::instance(backend);
        OutboxPostSource source(
            raw, service, channel.id, QString(), &backend);

        const QString first = service.enqueue(
            channel, QStringLiteral("one"), {}, {}, {},
            QStringLiteral("pending-a"));
        const QString second = service.enqueue(
            channel, QStringLiteral("two"), {}, {}, {},
            QStringLiteral("pending-b"));
        const QString third = service.enqueue(
            channel, QStringLiteral("three"), {}, {}, {},
            QStringLiteral("pending-c"));

        QCOMPARE(source.itemCount(), 3);
        QCOMPARE(source.postIdAt(0), first);
        QCOMPARE(source.postIdAt(1), second);
        QCOMPARE(source.postIdAt(2), third);

        raw.appendCoarse(QStringLiteral("remote"), 5000);
        QCOMPARE(source.postIdAt(0), QStringLiteral("remote"));
        QCOMPARE(source.postIdAt(1), first);
        QCOMPARE(source.postIdAt(2), second);
        QCOMPARE(source.postIdAt(3), third);

        service.cancel(first);
        QCOMPARE(source.postIdAt(1), second);
        QCOMPARE(source.postIdAt(2), third);

        QVERIFY(service.cancel(second));
        QVERIFY(service.cancel(third));
        QCOMPARE(source.itemCount(), 1);
    }
};

QTEST_MAIN(OutboxPostSourceTest)
#include "OutboxPostSourceTest.moc"
