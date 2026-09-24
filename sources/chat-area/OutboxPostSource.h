#pragma once

#include <memory>

#include <QHash>
#include <QPointer>
#include <QString>
#include <QVector>

#include "AbstractPostSource.h"

namespace Mattermost {

struct PendingPost;
class PendingPostService;

/**
 * Additive presentation projection over an authoritative post source.
 *
 * The wrapped source keeps the complete server coordinate system. Local
 * optimistic/failed outbox rows are appended as a stable presentation-only
 * tail and never enter BackendChannel, PostRepository, paging, thread counts,
 * read cursors, or server navigation.
 */
class OutboxPostSource final : public AbstractPostSource
{
    Q_OBJECT
public:
    OutboxPostSource(AbstractPostSource& source,
                     PendingPostService& outbox,
                     QString channelId,
                     QString rootId,
                     QObject* parent = nullptr);

    int itemCount() const override;
    bool isAvailable(int index) const override;
    BackendPost* postAt(int index) const override;
    QString postIdAt(int index) const override;
    int indexOfPost(const QString& postId) const override;
    int ensurePostIndex(const QString& postId) override;

    void requestRange(int first,
                      int last,
                      RequestReason reason,
                      quint64 generation) override;
    bool canRequestBeforeFirst() const override;
    void requestBeforeFirst(RequestReason reason, quint64 generation) override;

    int authoritativeItemCount() const override
    {
        return source ? source->authoritativeItemCount() : 0;
    }
    bool isAuthoritativeRow(int index) const override
    {
        return source && index >= 0 && index < sourceCount
            && source->isAuthoritativeRow(index);
    }
    bool isPostPositionAuthoritative(const QString& postId) const override
    {
        return source && source->isPostPositionAuthoritative(postId);
    }

    bool isPendingIndex(int index) const;
    bool isPendingPostId(const QString& postId) const;
    const PendingPost* pendingPostAt(int index) const;
    const PendingPost* pendingPost(const QString& postId) const;
    std::shared_ptr<BackendPost> pendingSnapshotAt(int index) const;
    AbstractPostSource* wrappedSource() const override { return source.data(); }

signals:
    void pendingPresentationChanged(const QString& pendingPostId);
    void pendingPromoted(int index,
                         const QString& pendingPostId,
                         const QString& serverPostId);

private:
    struct PendingRequest {
        int sourceFirst = -1;
        int sourceLast = -1;
        int requestedFirst = -1;
        int requestedLast = -1;
    };

    void handleSourceItemCountChanged(int count);
    void scheduleSourceStructureSync();
    void syncSourceStructure();
    void emitSourceAvailableRuns(int first, int last);
    bool sourceBoundaryMatchesPendingHead() const;
    bool tryPromotePending(const QString& pendingPostId,
                           const QString& serverPostId);
    void handlePendingConfirmed(const QString& pendingPostId,
                                const QString& serverPostId);
    void handlePendingAdded(const QString& changedChannelId,
                            const QString& changedRootId,
                            const QString& pendingPostId);
    void handlePendingChanged(const QString& changedChannelId,
                              const QString& changedRootId,
                              const QString& pendingPostId);
    void handlePendingRemoved(const QString& changedChannelId,
                              const QString& changedRootId,
                              const QString& pendingPostId);
    bool matchesConversation(const QString& changedChannelId,
                             const QString& changedRootId) const;

    QPointer<AbstractPostSource> source;
    PendingPostService& outbox;
    const QString channelId;
    const QString rootId;
    int sourceCount = 0;
    QVector<QString> pendingIds;
    QVector<PendingRequest> pendingRequests;
    QHash<QString, QString> confirmedServerIds;
    bool sourceStructureSyncScheduled = false;
};

} // namespace Mattermost
