#include "PendingPostService.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QVariant>

#include "Backend.h"
#include "DraftService.h"
#include "NetworkRequest.h"
#include "PostCreateService.h"
#include "PostProps.h"
#include "Storage.h"
#include "types/BackendChannel.h"
#include "types/BackendPost.h"
#include "chat-area/QuotedReplyFormat.h"

namespace Mattermost {
namespace {

constexpr qint64 RetryDelaysMs[] = {
    5 * 1000,
    20 * 1000,
    60 * 1000,
    180 * 1000,
    300 * 1000,
};

QString pendingStoreIdentity(Backend& backend)
{
    const QString userId = backend.getLoginUser().id;
    if (userId.isEmpty()) {
        return {};
    }
    QByteArray material = NetworkRequest::host().toUtf8();
    material.append('\0');
    material.append(userId.toUtf8());
    return QString::fromLatin1(
        QCryptographicHash::hash(material, QCryptographicHash::Sha256).toHex());
}

QString conversationKey(const QString& channelId, const QString& rootId)
{
    return channelId + QChar(0x1f) + rootId;
}

} // namespace

PendingPostService& PendingPostService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<PendingPostService>> instances;
    QPointer<PendingPostService>& service = instances[&backend];
    if (!service) {
        service = new PendingPostService(backend);
    }
    return *service;
}

PendingPostService::PendingPostService(Backend& backendInstance)
    : QObject(&backendInstance)
    , backend(backendInstance)
{
    ensureStorePath();
    loadStore();

    connect(&backend, &Backend::onNewPost, this,
            [this](BackendChannel& channel, const BackendPost& post) {
        handleAuthoritativePost(channel, post);
    });
}

PendingPostService::~PendingPostService()
{
    if (!storePath.isEmpty()
        && (!posts.empty() || !recoveryBacklog.isEmpty()
            || persistRetryScheduled)) {
        persist();
    }
}

void PendingPostService::setVisibilityPredicate(
    const QString& channelId,
    const QString& rootId,
    VisibilityPredicate predicate)
{
    if (channelId.isEmpty()) {
        return;
    }

    const QString key = conversationKey(channelId, rootId);
    if (predicate) {
        visibilityPredicates.insert(key, std::move(predicate));
    } else {
        visibilityPredicates.remove(key);
    }
}

QString PendingPostService::enqueue(BackendChannel& channel,
                                    const QString& wireMessage,
                                    const QList<QString>& attachmentIds,
                                    const QString& rootId,
                                    const QJsonObject& props,
                                    const QString& pendingPostId)
{
    // Durable optimistic recovery is intentionally text-only for now.
    // Attachment-bearing sends stay on the acknowledged path until uploaded
    // file IDs can be restored into an editable Draft after restart.
    if (channel.id.isEmpty() || pendingPostId.isEmpty()
        || wireMessage.isEmpty() || !attachmentIds.isEmpty()) {
        return {};
    }
    if (pendingPost(pendingPostId)) {
        return pendingPostId;
    }

    observeChannel(channel);

    auto post = std::make_unique<PendingPost>();
    post->pendingPostId = pendingPostId;
    post->channelId = channel.id;
    post->rootId = rootId;
    post->wireMessage = wireMessage;
    post->props = props;
    post->attachmentIds = attachmentIds;
    post->createdAt = QDateTime::currentMSecsSinceEpoch();
    post->retryWindowStartedAt = post->createdAt;
    post->authoritativeTailAtEnqueue =
        conversationTailAtEnqueue(channel, rootId);
    post->state = PendingPostState::Queued;
    rebuildSnapshot(*post);

    const QString id = post->pendingPostId;
    const qint64 retryWindowStartedAt = post->retryWindowStartedAt;
    posts.push_back(std::move(post));
    postsById.insert(id, posts.back().get());
    order.push_back(id);
    if (!persist()) {
        order.removeAll(id);
        postsById.remove(id);
        posts.erase(
            std::remove_if(
                posts.begin(), posts.end(),
                [&id](const std::unique_ptr<PendingPost>& candidate) {
                    return candidate && candidate->pendingPostId == id;
                }),
            posts.end());
        return {};
    }

    emit postAdded(channel.id, rootId, id);

    QPointer<PendingPostService> guard(this);
    QTimer::singleShot(0, this, [guard, channelId = channel.id, rootId] {
        if (guard) {
            guard->tryStartConversation(channelId, rootId);
        }
    });

    scheduleRetryDeadline(id, retryWindowStartedAt);

    return id;
}

QVector<QString> PendingPostService::pendingIds(const QString& channelId,
                                                const QString& rootId) const
{
    QVector<QString> result;
    result.reserve(order.size());
    for (const QString& id : order) {
        const PendingPost* post = pendingPost(id);
        if (post && post->channelId == channelId && post->rootId == rootId) {
            result.push_back(id);
        }
    }
    return result;
}

const PendingPost* PendingPostService::pendingPost(const QString& pendingPostId) const
{
    return postsById.value(pendingPostId, nullptr);
}

PendingPost* PendingPostService::mutablePost(const QString& pendingPostId)
{
    return postsById.value(pendingPostId, nullptr);
}

BackendPost* PendingPostService::snapshot(const QString& pendingPostId) const
{
    const PendingPost* post = pendingPost(pendingPostId);
    return post && post->snapshot ? post->snapshot.get() : nullptr;
}

std::shared_ptr<BackendPost> PendingPostService::snapshotLease(
    const QString& pendingPostId) const
{
    const PendingPost* post = pendingPost(pendingPostId);
    return post ? post->snapshot : std::shared_ptr<BackendPost>();
}

QString PendingPostService::firstPendingId(const QString& channelId,
                                           const QString& rootId) const
{
    for (const QString& id : order) {
        const PendingPost* post = pendingPost(id);
        if (post && post->channelId == channelId && post->rootId == rootId) {
            return id;
        }
    }
    return {};
}

void PendingPostService::rebuildSnapshot(PendingPost& post)
{
    QJsonObject raw {
        {QStringLiteral("id"), post.pendingPostId},
        {QStringLiteral("pending_post_id"), post.pendingPostId},
        {QStringLiteral("channel_id"), post.channelId},
        {QStringLiteral("root_id"), post.rootId},
        {QStringLiteral("message"), post.wireMessage},
        {QStringLiteral("props"), post.props},
        {QStringLiteral("create_at"), QJsonValue::fromVariant(post.createdAt)},
        {QStringLiteral("update_at"), QJsonValue::fromVariant(post.createdAt)},
        {QStringLiteral("user_id"), backend.getLoginUser().id},
        {QStringLiteral("_mmqt_sender_name"), backend.getLoginUser().getDisplayName()},
    };
    post.snapshot = std::make_shared<BackendPost>(raw, backend.getStorage());
}

quint64 PendingPostService::conversationTailAtEnqueue(
    BackendChannel& channel,
    const QString& rootId) const
{
    if (rootId.isEmpty()) {
        return channel.last_root_post_at;
    }

    BackendPost* root = channel.postIdToPost.value(rootId, nullptr);
    return root ? root->last_reply_at : 0;
}

void PendingPostService::observeChannel(BackendChannel& channel)
{
    if (observedChannels.contains(&channel)) {
        return;
    }
    observedChannels.insert(&channel);

    connect(&channel, &BackendChannel::onNewPosts, this,
            [this, guard = QPointer<BackendChannel>(&channel)](
                const ChannelNewPosts& collection) {
        if (!guard) {
            return;
        }
        for (const ChannelNewPostsChunk& chunk : collection.postsToAdd) {
            for (BackendPost* post : chunk.postsToAdd) {
                if (post) {
                    handleAuthoritativePost(*guard, *post);
                }
            }
        }
    });
    connect(&channel, &QObject::destroyed, this, [this, ptr = &channel] {
        observedChannels.remove(ptr);
    });
}

void PendingPostService::tryStartConversation(const QString& channelId,
                                              const QString& rootId)
{
    const QVector<QString> ids = pendingIds(channelId, rootId);
    if (ids.isEmpty()) {
        return;
    }

    PendingPost* head = mutablePost(ids.first());
    if (!head) {
        return;
    }

    QVector<PendingPost*> newlyBlocked;
    for (int i = 1; i < ids.size(); ++i) {
        PendingPost* later = mutablePost(ids.at(i));
        if (!later || later->state == PendingPostState::Failed) {
            continue;
        }
        if (later->state != PendingPostState::Blocked) {
            later->state = PendingPostState::Blocked;
            later->failureText.clear();
            ++later->generation;
            newlyBlocked.push_back(later);
        }
    }
    if (!newlyBlocked.isEmpty()) {
        for (PendingPost* later : newlyBlocked) {
            emit postChanged(later->channelId, later->rootId,
                             later->pendingPostId);
        }
    }

    if (head->state == PendingPostState::Failed
        || head->state == PendingPostState::Sending
        || head->state == PendingPostState::RetryWait) {
        return;
    }

    if (head->interveningPostCount >= MaxInterveningPosts) {
        fail(*head, tr("Conversation advanced while this message was pending"));
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() - head->retryWindowStartedAt >= RetryWindowMs) {
        fail(*head, tr("Sending timed out after 10 minutes"));
        return;
    }

    head->state = PendingPostState::Queued;
    head->failureText.clear();
    ++head->generation;
    const quint64 generation = head->generation;
    emit postChanged(head->channelId, head->rootId, head->pendingPostId);

    const QString id = head->pendingPostId;
    QPointer<PendingPostService> guard(this);
    QTimer::singleShot(0, this, [guard, id, generation] {
        PendingPost* current = guard ? guard->mutablePost(id) : nullptr;
        if (current && current->generation == generation
            && current->state == PendingPostState::Queued) {
            guard->attempt(id);
        }
    });
}

void PendingPostService::attempt(const QString& pendingPostId)
{
    PendingPost* post = mutablePost(pendingPostId);
    if (!post || firstPendingId(post->channelId, post->rootId) != pendingPostId) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (post->attemptCount >= MaxAttempts) {
        fail(*post, tr("Maximum send attempts reached"));
        return;
    }
    if (now - post->retryWindowStartedAt >= RetryWindowMs) {
        fail(*post, tr("Sending timed out after 10 minutes"));
        return;
    }
    if (post->interveningPostCount >= MaxInterveningPosts) {
        fail(*post, tr("Conversation advanced while this message was pending"));
        return;
    }

    BackendChannel* channel = backend.getStorage().getChannelById(post->channelId);
    if (!channel) {
        fail(*post, tr("Conversation is no longer available"));
        return;
    }

    ++post->attemptCount;
    post->state = PendingPostState::Sending;
    post->failureText.clear();
    const quint64 requestGeneration = ++post->generation;
    emit postChanged(post->channelId, post->rootId, post->pendingPostId);

    const QString id = post->pendingPostId;
    QPointer<PendingPostService> guard(this);
    PostCreateService::instance(backend).createPostDetailed(
        *channel,
        post->wireMessage,
        post->attachmentIds,
        post->rootId,
        post->props,
        post->pendingPostId,
        [guard, id, requestGeneration](PostCreateService::CreatePostResult result) {
        PendingPostService* service = guard.data();
        if (!service) {
            return;
        }

        PendingPost* current = service->mutablePost(id);
        if (!current) {
            return;
        }
        if (result.post) {
            service->confirm(id, result.post->id);
            return;
        }
        if (current->generation != requestGeneration
            || current->state == PendingPostState::Failed) {
            return;
        }

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const bool attemptsRemaining =
            current->attemptCount < MaxAttempts;
        const bool timeRemaining =
            now - current->retryWindowStartedAt < RetryWindowMs;
        const bool conversationStillCurrent =
            current->interveningPostCount < MaxInterveningPosts;

        if (result.retryable() && attemptsRemaining && timeRemaining
            && conversationStillCurrent) {
            service->scheduleRetry(*current);
            return;
        }

        if (!conversationStillCurrent) {
            service->fail(
                *current,
                service->tr("Three newer messages arrived before this message was sent"));
            return;
        }
        if (!timeRemaining) {
            service->fail(*current,
                          service->tr("Sending timed out after 10 minutes"));
            return;
        }
        if (!attemptsRemaining && result.retryable()) {
            service->fail(*current,
                          service->tr("Maximum send attempts reached"));
            return;
        }

        QString reason = result.errorText.trimmed();
        if (reason.isEmpty() && result.httpStatus > 0) {
            reason = service->tr("Server returned HTTP %1").arg(result.httpStatus);
        }
        if (reason.isEmpty()) {
            reason = service->tr("Message could not be sent");
        }
        service->fail(*current, reason);
    });
}

void PendingPostService::scheduleRetry(PendingPost& post)
{
    if (post.attemptCount <= 0 || post.attemptCount >= MaxAttempts) {
        fail(post, tr("Maximum send attempts reached"));
        return;
    }

    const int delayIndex = std::min(
        post.attemptCount - 1,
        static_cast<int>(sizeof(RetryDelaysMs) / sizeof(RetryDelaysMs[0])) - 1);
    qint64 delay = RetryDelaysMs[delayIndex];
    const qint64 remaining =
        RetryWindowMs - (QDateTime::currentMSecsSinceEpoch() - post.retryWindowStartedAt);
    if (remaining <= 0) {
        fail(post, tr("Sending timed out after 10 minutes"));
        return;
    }
    delay = std::min(delay, remaining);

    post.state = PendingPostState::RetryWait;
    post.failureText.clear();
    const quint64 generation = ++post.generation;
    const QString id = post.pendingPostId;
    emit postChanged(post.channelId, post.rootId, id);

    QPointer<PendingPostService> guard(this);
    QTimer::singleShot(static_cast<int>(delay), this,
                       [guard, id, generation] {
        PendingPost* current = guard ? guard->mutablePost(id) : nullptr;
        if (!current || current->generation != generation
            || current->state != PendingPostState::RetryWait) {
            return;
        }
        current->state = PendingPostState::Queued;
        ++current->generation;
        emit guard->postChanged(current->channelId, current->rootId, id);
        guard->tryStartConversation(current->channelId, current->rootId);
    });
}

void PendingPostService::scheduleRetryDeadline(
    const QString& pendingPostId,
    qint64 retryWindowStartedAt)
{
    QPointer<PendingPostService> guard(this);
    QTimer::singleShot(
        static_cast<int>(RetryWindowMs), this,
        [guard, pendingPostId, retryWindowStartedAt] {
        PendingPost* current =
            guard ? guard->mutablePost(pendingPostId) : nullptr;
        if (!current || current->state == PendingPostState::Failed
            || current->retryWindowStartedAt != retryWindowStartedAt) {
            return;
        }

        // The time budget stops future retries; it cannot revoke an HTTP
        // request that may already have reached the server. Keep an in-flight
        // head non-cancellable until that request resolves, then either confirm
        // it or fail it without scheduling another attempt.
        if (current->state == PendingPostState::Sending) {
            return;
        }
        guard->fail(*current, guard->tr("Sending timed out after 10 minutes"));
    });
}

void PendingPostService::fail(PendingPost& post, const QString& reason)
{
    post.state = PendingPostState::Failed;
    post.failureText = reason;
    ++post.generation;
    emit postChanged(post.channelId, post.rootId, post.pendingPostId);
}

bool PendingPostService::retry(const QString& pendingPostId)
{
    PendingPost* post = mutablePost(pendingPostId);
    if (!post || post->state != PendingPostState::Failed) {
        return false;
    }

    post->attemptCount = 0;
    post->interveningPostCount = 0;
    post->interveningPostIds.clear();
    post->retryWindowStartedAt = QDateTime::currentMSecsSinceEpoch();
    if (BackendChannel* channel =
            backend.getStorage().getChannelById(post->channelId)) {
        post->authoritativeTailAtEnqueue =
            conversationTailAtEnqueue(*channel, post->rootId);
        observeChannel(*channel);
    }
    post->state = PendingPostState::Queued;
    post->failureText.clear();
    ++post->generation;
    emit postChanged(post->channelId, post->rootId, post->pendingPostId);
    scheduleRetryDeadline(post->pendingPostId, post->retryWindowStartedAt);

    const QString channelId = post->channelId;
    const QString rootId = post->rootId;
    QPointer<PendingPostService> guard(this);
    QTimer::singleShot(0, this, [guard, channelId, rootId] {
        if (guard) {
            guard->tryStartConversation(channelId, rootId);
        }
    });
    return true;
}

bool PendingPostService::cancel(const QString& pendingPostId)
{
    if (!pendingPost(pendingPostId)) {
        return false;
    }
    remove(pendingPostId, true);
    return true;
}

void PendingPostService::confirm(const QString& pendingPostId,
                                 const QString& serverPostId)
{
    if (!pendingPost(pendingPostId)) {
        return;
    }

    if (!serverPostId.isEmpty()) {
        confirmedServerPostIds.insert(serverPostId);

        // HTTP acknowledgement may win the race with websocket delivery. Keep
        // the server ID only for the maximum retry horizon: after that, an old
        // echo is older than any still-eligible automatic retry anyway.
        QPointer<PendingPostService> guard(this);
        QTimer::singleShot(static_cast<int>(RetryWindowMs), this,
                           [guard, serverPostId] {
            if (guard) {
                guard->confirmedServerPostIds.remove(serverPostId);
            }
        });
    }

    emit postConfirmed(pendingPostId, serverPostId);
    remove(pendingPostId, true);
}

void PendingPostService::remove(const QString& pendingPostId, bool startNext)
{
    PendingPost* current = mutablePost(pendingPostId);
    if (!current) {
        return;
    }
    const QString channelId = current->channelId;
    const QString rootId = current->rootId;

    // Presentation consumers hold a reference to the synthetic BackendPost.
    // Publish the structural removal while that snapshot is still alive so
    // LongListWidget can destroy the materialized row synchronously first.
    emit postRemoved(channelId, rootId, pendingPostId);

    order.removeAll(pendingPostId);
    postsById.remove(pendingPostId);
    posts.erase(
        std::remove_if(posts.begin(), posts.end(),
                       [&pendingPostId](const std::unique_ptr<PendingPost>& post) {
            return post && post->pendingPostId == pendingPostId;
        }),
        posts.end());
    persistBestEffort();

    if (startNext) {
        QPointer<PendingPostService> guard(this);
        QTimer::singleShot(0, this, [guard, channelId, rootId] {
            if (guard) {
                guard->tryStartConversation(channelId, rootId);
            }
        });
    }
}

void PendingPostService::handleAuthoritativePost(
    BackendChannel& channel,
    const BackendPost& post)
{
    if (!post.id.isEmpty() && confirmedServerPostIds.contains(post.id)) {
        // Most commonly this is the websocket echo of a post already ingested
        // and confirmed by the HTTP response. It is the same authoritative
        // message, not conversation progress past the next queued local post.
        return;
    }

    if (!post.pending_post_id.isEmpty()
        && pendingPost(post.pending_post_id)) {
        // This is confirmation of one of our own optimistic rows. It advances
        // authoritative topology but is expected FIFO progress, not a
        // conversation-advanced penalty for later queued messages.
        confirm(post.pending_post_id, post.id);
        return;
    }

    const QString logicalRoot = post.root_id;
    const QVector<QString> ids = pendingIds(channel.id, logicalRoot);
    if (ids.isEmpty() || post.id.isEmpty()
        || !isVisibleAuthoritativePost(channel.id, logicalRoot, post)) {
        return;
    }

    bool changed = false;
    PendingPost* failedHead = nullptr;
    for (const QString& id : ids) {
        PendingPost* pending = mutablePost(id);
        if (!pending
            || pending->interveningPostIds.contains(post.id)
            || (pending->authoritativeTailAtEnqueue > 0
                && post.create_at <= pending->authoritativeTailAtEnqueue)) {
            continue;
        }

        pending->interveningPostIds.insert(post.id);
        pending->interveningPostCount =
            pending->interveningPostIds.size();
        changed = true;

        if (id == ids.first()
            && pending->state != PendingPostState::Failed
            && pending->state != PendingPostState::Sending
            && pending->interveningPostCount >= MaxInterveningPosts) {
            pending->state = PendingPostState::Failed;
            pending->failureText =
                tr("Three newer messages arrived before this message was sent");
            ++pending->generation;
            failedHead = pending;
        }
    }

    if (!changed) {
        return;
    }

    // Retry/cutoff state is session-only. The durable file stores unresolved
    // message intent, not transport state, because restart never resumes it.
    if (failedHead) {
        emit postChanged(failedHead->channelId, failedHead->rootId,
                         failedHead->pendingPostId);
    }
}

bool PendingPostService::isVisibleAuthoritativePost(
    const QString& channelId,
    const QString& rootId,
    const BackendPost& post) const
{
    const auto it = visibilityPredicates.constFind(
        conversationKey(channelId, rootId));
    return it == visibilityPredicates.cend() || !it.value()
        || it.value()(post);
}

void PendingPostService::persistBestEffort()
{
    if (persist()) {
        persistRetryScheduled = false;
        return;
    }
    if (persistRetryScheduled) {
        return;
    }

    persistRetryScheduled = true;
    QPointer<PendingPostService> guard(this);
    QTimer::singleShot(5000, this, [guard] {
        if (!guard) {
            return;
        }
        guard->persistRetryScheduled = false;
        guard->persistBestEffort();
    });
}

QString PendingPostService::stateText(PendingPostState state,
                                      int attemptCount,
                                      int interveningPostCount,
                                      const QString& failureText)
{
    switch (state) {
    case PendingPostState::Queued:
        return QObject::tr("Queued");
    case PendingPostState::Sending:
        return attemptCount > 1
            ? QObject::tr("Sending · attempt %1 of %2")
                  .arg(attemptCount).arg(MaxAttempts)
            : QObject::tr("Sending…");
    case PendingPostState::RetryWait:
        return QObject::tr("Waiting to retry · attempt %1 of %2")
            .arg(attemptCount).arg(MaxAttempts);
    case PendingPostState::Blocked:
        return QObject::tr("Waiting for previous message");
    case PendingPostState::Failed:
        if (!failureText.isEmpty()) {
            return QObject::tr("Failed · %1").arg(failureText);
        }
        if (interveningPostCount >= MaxInterveningPosts) {
            return QObject::tr("Failed · conversation moved on");
        }
        return QObject::tr("Failed to send");
    }
    return {};
}

void PendingPostService::ensureStorePath()
{
    const QString identity = pendingStoreIdentity(backend);
    if (identity.isEmpty()) {
        return;
    }
    const QString base =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    storePath = QDir(base).filePath(
        QStringLiteral("outbox/") + identity + QStringLiteral(".json"));
}

void PendingPostService::loadStore()
{
    if (storePath.isEmpty()) {
        return;
    }

    QFile file(storePath);
    if (!file.exists()) {
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &error);
    file.close();
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != 1) {
        return;
    }

    // A process boundary is a hard auto-resend boundary. Never wake up and
    // transmit old messages on the user's behalf. Move every durable outbox
    // item into the local Drafts collection instead, keeping one recovered
    // draft per former pending_post_id so multiple unsent messages from the
    // same conversation remain distinct and in user control.
    auto& drafts = DraftService::instance(backend);
    int failedRecoveryIndex = -1;
    const QJsonArray array = root.value(QStringLiteral("posts")).toArray();
    for (int index = 0; index < array.size(); ++index) {
        const QJsonValue value = array.at(index);
        const QJsonObject object = value.toObject();
        const QString pendingPostId =
            object.value(QStringLiteral("pending_post_id")).toString();
        const QString channelId =
            object.value(QStringLiteral("channel_id")).toString();
        const QString rootId =
            object.value(QStringLiteral("root_id")).toString();
        const QString wireMessage =
            object.value(QStringLiteral("message")).toString();
        const QJsonObject props =
            object.value(QStringLiteral("props")).toObject();
        const QString replyToPostId =
            props.value(QString::fromLatin1(PostProps::ReplyToPostId))
                .toString();

        if (pendingPostId.isEmpty() || channelId.isEmpty()
            || wireMessage.isEmpty()) {
            continue;
        }

        const QString message = replyToPostId.isEmpty()
            ? wireMessage
            : QuotedReplyFormat::stripFallback(wireMessage);
        if (message.isEmpty()) {
            continue;
        }

        if (drafts.addRecoveredDraft(
                channelId, rootId, message, replyToPostId, pendingPostId)
            .isEmpty()) {
            failedRecoveryIndex = index;
            break;
        }

        // A crash can happen after the outbox handoff is durable but just
        // before finishSend() removes the ordinary composer draft. Once the
        // recovered entry is durable, discard only that exact stale duplicate.
        // A different ordinary draft in the same conversation remains intact.
        DraftEntry ordinaryDraft;
        if (drafts.findDraft(channelId, rootId, ordinaryDraft)
            && ordinaryDraft.message == message
            && ordinaryDraft.replyToPostId == replyToPostId) {
            drafts.removeDraft(channelId, rootId);
        }
    }

    // Each successful prefix entry is already durable in DraftStore and must
    // leave the outbox permanently; otherwise sending/deleting that recovered
    // draft in this process could make it reappear on the next restart. Keep
    // only the failed/unprocessed suffix as the recovery backlog.
    recoveryBacklog.clear();
    if (failedRecoveryIndex < 0) {
        QFile::remove(storePath);
    } else {
        recoveryBacklog.reserve(array.size() - failedRecoveryIndex);
        for (int index = failedRecoveryIndex; index < array.size(); ++index) {
            const QJsonValue value = array.at(index);
            if (value.isObject()) {
                recoveryBacklog.push_back(value.toObject());
            }
        }
    }
}

bool PendingPostService::persist() const
{
    if (storePath.isEmpty()) {
        return false;
    }

    const QFileInfo info(storePath);
    if (!QDir().mkpath(info.absolutePath())) {
        return false;
    }

    if (order.isEmpty() && recoveryBacklog.isEmpty()) {
        return !QFileInfo::exists(storePath) || QFile::remove(storePath);
    }

    QJsonArray array;
    for (const QJsonObject& recovered : recoveryBacklog) {
        array.push_back(recovered);
    }
    for (const QString& id : order) {
        const PendingPost* post = pendingPost(id);
        if (!post) {
            continue;
        }
        QJsonArray fileIds;
        for (const QString& fileId : post->attachmentIds) {
            fileIds.push_back(fileId);
        }
        array.push_back(QJsonObject {
            {QStringLiteral("pending_post_id"), post->pendingPostId},
            {QStringLiteral("channel_id"), post->channelId},
            {QStringLiteral("root_id"), post->rootId},
            {QStringLiteral("message"), post->wireMessage},
            {QStringLiteral("props"), post->props},
            {QStringLiteral("file_ids"), fileIds},
            {QStringLiteral("created_at"),
             QJsonValue::fromVariant(post->createdAt)},
        });
    }

    QSaveFile file(storePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    const QJsonDocument document(QJsonObject {
        {QStringLiteral("version"), 1},
        {QStringLiteral("posts"), array},
    });
    if (file.write(document.toJson(QJsonDocument::Compact)) < 0) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

} // namespace Mattermost
