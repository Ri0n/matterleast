#pragma once

#include <functional>

#include <QPointer>
#include <QSet>
#include <QVector>

#include "AbstractPostSource.h"

namespace Mattermost {

/**
 * Predicate-filtered projection over an arbitrary post source.
 *
 * The wrapped source remains authoritative for transport, absolute logical
 * positions and semantic identity. This layer exposes a second logical
 * coordinate system that simply omits rows rejected by the predicate.
 *
 * Unresolved/non-resident rows are kept in the projection until their body can
 * be classified. A rejected identity remains rejected across body eviction, so
 * filtering does not depend on the residency cache.
 */
class FilteredPostSource final : public AbstractPostSource
{
    Q_OBJECT
public:
    using Predicate = std::function<bool(const BackendPost&)>;

    explicit FilteredPostSource(AbstractPostSource& source,
                                Predicate predicate,
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

    /** Replace the predicate and re-project all currently resident rows. */
    void setPredicate(Predicate predicate);

    /**
     * Drop the cached decision for one post and re-evaluate it when possible.
     * Predicates over immutable fields never need to call this.
     */
    void invalidatePost(const QString& postId);

    AbstractPostSource* wrappedSource() const override { return source.data(); }
    bool isPostPositionAuthoritative(const QString& postId) const override
    {
        return source && source->isPostPositionAuthoritative(postId);
    }

private:
    struct PendingRequest {
        int sourceFirst = -1;
        int sourceLast = -1;
        int filteredFirst = -1;
        int filteredLast = -1;
    };

    void evaluateRange(int first, int last);
    QVector<int> currentRejectedSourceIndices() const;
    void applyRejectedSourceIndices(const QVector<int>& target);
    void emitLayoutForSourceRange(int first, int last);

    void handleItemCountChanged(int count);
    void handleItemsInserted(int first, int count);
    void handleItemsRemoved(int first, int count);
    void handleRangeAvailable(int first, int last);
    void handleBodyAvailabilityChanged(int first, int last, bool available);
    void handleLayoutChanged(int first, int last);
    void handleSeekTargetResolved(int sourceIndex, quint64 generation);
    void handleRangeRequestFinished(int first, int last);

    void emitAvailableRuns(int sourceFirst, int sourceLast);
    void emitBodyAvailabilityRuns(int sourceFirst, int sourceLast, bool available);

    bool isRejectedSourceIndex(int sourceIndex) const;
    int rejectedBefore(int sourceIndex) const;
    int filteredIndexForSource(int sourceIndex) const;
    int sourceIndexForFiltered(int index) const;
    int filteredInsertionIndexForSource(int sourceIndex) const;

    QPointer<AbstractPostSource> source;
    Predicate predicate;

    // Cache only rejected identities. Accepted and unresolved rows have the same
    // projection behavior, which keeps this adapter sparse even for huge chats.
    QSet<QString> rejectedPostIds;
    QVector<int> rejectedSourceIndices;
    int sourceCount = 0;

    QVector<PendingRequest> pendingRequests;
};

} // namespace Mattermost
