#pragma once

#include <functional>

#include <QHash>
#include <QPointer>
#include <QVector>

#include "AbstractPostSource.h"

namespace Mattermost {

/**
 * Predicate-filtered projection over an arbitrary post source.
 *
 * The wrapped source remains the authority for transport, absolute logical
 * positions and semantic identity. This layer only exposes a filtered logical
 * coordinate system to its consumer. Rows whose identity/body is not resolved
 * yet stay provisionally visible so the consumer can request them; once a body
 * becomes available the predicate is evaluated before that row is published as
 * available.
 *
 * A false predicate result removes the row from this projection only. It never
 * mutates the wrapped source.
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

    /**
     * Replace the predicate and re-project every row that can currently be
     * classified. Rows with non-resident bodies become unresolved until loaded.
     */
    void setPredicate(Predicate predicate);

    /**
     * Forget one cached predicate result and re-evaluate it when possible.
     * Useful for predicates that depend on mutable post data.
     */
    void invalidatePost(const QString& postId);

    AbstractPostSource* wrappedSource() const { return source.data(); }

private:
    enum class Decision {
        Unknown,
        Accepted,
        Rejected,
    };

    struct SourceRow {
        QString postId;
        Decision decision = Decision::Unknown;
    };

    struct PendingRequest {
        int sourceFirst = -1;
        int sourceLast = -1;
        int filteredFirst = -1;
        int filteredLast = -1;
    };

    SourceRow resolvedRow(int sourceIndex);
    bool isIncluded(const SourceRow& row) const;
    void rebuildProjection();

    /**
     * Re-read identity/body state for a source range. Visibility transitions are
     * emitted incrementally so the consumer and this projection have matching
     * coordinates after every structural signal.
     */
    void synchronizeRange(int first, int last, bool forceLayoutSignal = false);

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

    int sourceIndexForFiltered(int index) const;
    int filteredInsertionIndexForSource(int sourceIndex) const;

    QPointer<AbstractPostSource> source;
    Predicate predicate;

    QVector<SourceRow> rows;
    QVector<int> filteredToSource;
    QVector<int> sourceToFiltered;
    QHash<QString, Decision> decisionsByPostId;
    QVector<PendingRequest> pendingRequests;
};

} // namespace Mattermost
