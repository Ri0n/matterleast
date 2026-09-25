#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "types/BackendPost.h"

namespace Mattermost {

class Backend;
class BackendChannel;

enum class PendingPostState {
    Uploading,
    Queued,
    Sending,
    RetryWait,
    Failed,
    Blocked,
};

struct PendingAttachment
{
    QString uploadId;
    QString path;
    QString fileId;
};

struct PendingPost
{
    QString pendingPostId;
    QString channelId;
    QString rootId;
    QString wireMessage;
    QJsonObject props;
    QList<PendingAttachment> attachments;

    qint64 createdAt = 0;
    qint64 retryWindowStartedAt = 0;
    int attemptCount = 0;
    int interveningPostCount = 0;
    quint64 authoritativeTailAtEnqueue = 0;
    QSet<QString> interveningPostIds;
    PendingPostState state = PendingPostState::Queued;
    QString failureText;
    quint64 generation = 0;

    std::shared_ptr<BackendPost> snapshot;
};

/**
 * Session outbox for optimistic posts.
 *
 * Pending posts are deliberately not inserted into BackendChannel or
 * PostRepository. They are client-side presentation data until Mattermost
 * confirms them with a real post carrying the same pending_post_id.
 *
 * Attachment-bearing operations are accepted before their uploads finish.
 * Backend-scoped upload ownership lets the composer release immediately while
 * this service waits for file IDs, preserves FIFO ordering, and then creates
 * the post. Restart recovery stores the local attachment paths in the recovered
 * draft rather than resuming network delivery automatically.
 *
 * Automatic delivery is FIFO per logical conversation (channel + root_id).
 * A head item retries for at most ten minutes, at most six attempts, and stops
 * early after three authoritative messages arrive in that same conversation.
 */
class PendingPostService final : public QObject
{
    Q_OBJECT
public:
    using VisibilityPredicate = std::function<bool(const BackendPost&)>;

    static PendingPostService& instance(Backend& backend);
    ~PendingPostService() override;

    void setVisibilityPredicate(const QString& channelId,
                                const QString& rootId,
                                VisibilityPredicate predicate);

    QString enqueue(BackendChannel& channel,
                    const QString& wireMessage,
                    const QList<PendingAttachment>& attachments,
                    const QString& rootId,
                    const QJsonObject& props,
                    const QString& pendingPostId);

    QVector<QString> pendingIds(const QString& channelId,
                                const QString& rootId) const;
    const PendingPost* pendingPost(const QString& pendingPostId) const;
    BackendPost* snapshot(const QString& pendingPostId) const;
    std::shared_ptr<BackendPost> snapshotLease(
        const QString& pendingPostId) const;

    bool retry(const QString& pendingPostId);
    bool cancel(const QString& pendingPostId);

    int attachmentUploadProgress(const PendingPost& post) const;

    static QString stateText(PendingPostState state, int attemptCount,
                             int interveningPostCount,
                             const QString& failureText,
                             int uploadPercent = -1);

    static constexpr int MaxAttempts = 6;
    static constexpr int MaxInterveningPosts = 3;
    static constexpr qint64 RetryWindowMs = 10 * 60 * 1000;

signals:
    void postAdded(const QString& channelId,
                   const QString& rootId,
                   const QString& pendingPostId);
    void postChanged(const QString& channelId,
                     const QString& rootId,
                     const QString& pendingPostId);
    void postRemoved(const QString& channelId,
                     const QString& rootId,
                     const QString& pendingPostId);
    void postConfirmed(const QString& pendingPostId,
                       const QString& serverPostId);

private:
    explicit PendingPostService(Backend& backend);

    PendingPost* mutablePost(const QString& pendingPostId);
    QString firstPendingId(const QString& channelId,
                           const QString& rootId) const;
    void rebuildSnapshot(PendingPost& post);
    void observeChannel(BackendChannel& channel);
    quint64 conversationTailAtEnqueue(BackendChannel& channel,
                                      const QString& rootId) const;
    void tryStartConversation(const QString& channelId,
                              const QString& rootId);
    void attempt(const QString& pendingPostId);
    void scheduleRetry(PendingPost& post);
    void scheduleRetryDeadline(const QString& pendingPostId,
                               qint64 retryWindowStartedAt);
    void fail(PendingPost& post, const QString& reason);
    void confirm(const QString& pendingPostId, const QString& serverPostId);
    void remove(const QString& pendingPostId, bool startNext);
    void handleAuthoritativePost(BackendChannel& channel,
                                 const BackendPost& post);
    void handleAttachmentUploadChanged(const QString& uploadId);
    void handleAttachmentUploadProgress(const QString& uploadId);
    bool attachmentsReady(const PendingPost& post) const;
    QList<QString> attachmentFileIds(const PendingPost& post) const;

    void ensureStorePath();
    void loadStore();
    bool persist() const;
    void persistBestEffort();
    bool isVisibleAuthoritativePost(const QString& channelId,
                                    const QString& rootId,
                                    const BackendPost& post) const;

    Backend& backend;
    std::vector<std::unique_ptr<PendingPost>> posts;
    QHash<QString, PendingPost*> postsById;
    QVector<QString> order;
    QHash<QString, VisibilityPredicate> visibilityPredicates;
    // If startup recovery cannot durably hand old records to DraftService,
    // preserve the original JSON verbatim in every subsequent outbox snapshot.
    // A later process can retry the idempotent recovery.
    QVector<QJsonObject> recoveryBacklog;
    QSet<BackendChannel*> observedChannels;
    // HTTP acknowledgement can ingest a post before its websocket echo arrives.
    // Remember recently confirmed server IDs so that echo is not counted as an
    // intervening conversation message for the next FIFO entry.
    QSet<QString> confirmedServerPostIds;
    QString storePath;
    bool persistRetryScheduled = false;
};

} // namespace Mattermost
