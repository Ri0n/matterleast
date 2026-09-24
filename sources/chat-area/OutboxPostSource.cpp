#include "OutboxPostSource.h"

#include <algorithm>
#include <utility>

#include <QTimer>

#include "backend/PendingPostService.h"

namespace Mattermost {

OutboxPostSource::OutboxPostSource(AbstractPostSource& sourceInstance,
                                   PendingPostService& outboxInstance,
                                   QString channelIdInstance,
                                   QString rootIdInstance,
                                   QObject* parent)
    : AbstractPostSource(parent)
    , source(&sourceInstance)
    , outbox(outboxInstance)
    , channelId(std::move(channelIdInstance))
    , rootId(std::move(rootIdInstance))
    , sourceCount(sourceInstance.itemCount())
    , pendingIds(outboxInstance.pendingIds(channelId, rootId))
{
    connect(&sourceInstance, &AbstractPostSource::itemCountChanged,
            this, &OutboxPostSource::handleSourceItemCountChanged);
    connect(&sourceInstance, &AbstractPostSource::itemsInserted,
            this, [this](int first, int count) {
        count = std::max(0, count);
        if (count <= 0) {
            return;
        }
        // Exact structural signals already carry the authoritative insertion
        // coordinate. Preserve them exactly; only coarse tail-count growth is
        // deferred for optimistic confirmation coalescing.
        sourceCount += count;
        emit itemsInserted(first, count);
    });
    connect(&sourceInstance, &AbstractPostSource::itemsRemoved,
            this, [this](int first, int count) {
        count = std::max(0, std::min(count, sourceCount));
        if (count <= 0) {
            return;
        }
        sourceCount -= count;
        emit itemsRemoved(first, count);
    });
    connect(&sourceInstance, &AbstractPostSource::rangeAvailable,
            this, [this](int first, int last) {
        if (pendingIds.isEmpty() || last < sourceCount) {
            emit rangeAvailable(first, last);
            return;
        }
        if (first < sourceCount) {
            emit rangeAvailable(first, sourceCount - 1);
        }

        // Once the wrapped source has installed the new concrete identity we
        // can distinguish ordinary incoming traffic from our own confirmation.
        // Ordinary traffic is published immediately; only the correlated FIFO
        // promotion waits for postConfirmed/postRemoved in this same call stack.
        if (sourceBoundaryMatchesPendingHead()) {
            scheduleSourceStructureSync();
        } else {
            syncSourceStructure();
        }
    });
    connect(&sourceInstance, &AbstractPostSource::bodyAvailabilityChanged,
            this, [this](int first, int last, bool available) {
        if (pendingIds.isEmpty() || last < sourceCount) {
            emit bodyAvailabilityChanged(first, last, available);
            return;
        }
        if (first < sourceCount) {
            emit bodyAvailabilityChanged(first, sourceCount - 1, available);
        }
        scheduleSourceStructureSync();
    });
    connect(&sourceInstance, &AbstractPostSource::seekTargetResolved,
            this, &OutboxPostSource::seekTargetResolved);
    connect(&sourceInstance, &AbstractPostSource::layoutChanged,
            this, [this](int first, int last) {
        if (first < sourceCount) {
            emit layoutChanged(first, std::min(last, sourceCount - 1));
        }
        if (last >= sourceCount) {
            scheduleSourceStructureSync();
        }
    });
    connect(&sourceInstance, &AbstractPostSource::rangeRequestFinished,
            this, [this](int first, int last) {
        for (int i = 0; i < pendingRequests.size(); ++i) {
            const PendingRequest request = pendingRequests.at(i);
            if (request.sourceFirst != first || request.sourceLast != last) {
                continue;
            }
            pendingRequests.removeAt(i);
            emit rangeRequestFinished(
                request.requestedFirst, request.requestedLast);
            return;
        }
        emit rangeRequestFinished(first, last);
    });
    connect(&sourceInstance, &QObject::destroyed, this, [this] {
        const int oldSourceCount = sourceCount;
        source.clear();
        sourceCount = 0;
        pendingRequests.clear();
        if (oldSourceCount > 0) {
            emit itemsRemoved(0, oldSourceCount);
        }
    });

    connect(&outbox, &PendingPostService::postAdded,
            this, &OutboxPostSource::handlePendingAdded);
    connect(&outbox, &PendingPostService::postConfirmed,
            this, &OutboxPostSource::handlePendingConfirmed);
    connect(&outbox, &PendingPostService::postChanged,
            this, &OutboxPostSource::handlePendingChanged);
    connect(&outbox, &PendingPostService::postRemoved,
            this, &OutboxPostSource::handlePendingRemoved);
}

int OutboxPostSource::itemCount() const
{
    return sourceCount + pendingIds.size();
}

bool OutboxPostSource::isPendingIndex(int index) const
{
    return index >= sourceCount
        && index < sourceCount + pendingIds.size();
}

bool OutboxPostSource::isAvailable(int index) const
{
    if (isPendingIndex(index)) {
        return pendingPostAt(index) != nullptr;
    }
    return source && index >= 0 && index < sourceCount
        && source->isAvailable(index);
}

BackendPost* OutboxPostSource::postAt(int index) const
{
    if (isPendingIndex(index)) {
        return outbox.snapshot(pendingIds.at(index - sourceCount));
    }
    return source && index >= 0 && index < sourceCount
        ? source->postAt(index) : nullptr;
}

QString OutboxPostSource::postIdAt(int index) const
{
    if (isPendingIndex(index)) {
        return pendingIds.at(index - sourceCount);
    }
    return source && index >= 0 && index < sourceCount
        ? source->postIdAt(index) : QString();
}

int OutboxPostSource::indexOfPost(const QString& postId) const
{
    if (postId.isEmpty()) {
        return -1;
    }
    if (source) {
        const int sourceIndex = source->indexOfPost(postId);
        if (sourceIndex >= 0) {
            return sourceIndex;
        }
    }
    const int pendingIndex = pendingIds.indexOf(postId);
    return pendingIndex >= 0 ? sourceCount + pendingIndex : -1;
}

int OutboxPostSource::ensurePostIndex(const QString& postId)
{
    const int pendingIndex = pendingIds.indexOf(postId);
    if (pendingIndex >= 0) {
        return sourceCount + pendingIndex;
    }
    return source ? source->ensurePostIndex(postId) : -1;
}

void OutboxPostSource::requestRange(int first,
                                    int last,
                                    RequestReason reason,
                                    quint64 generation)
{
    const int requestedFirst = first;
    const int requestedLast = last;

    if (itemCount() <= 0) {
        emit rangeRequestFinished(requestedFirst, requestedLast);
        return;
    }

    first = std::max(0, first);
    last = std::min(itemCount() - 1, last);
    if (last < first) {
        emit rangeRequestFinished(requestedFirst, requestedLast);
        return;
    }

    const int pendingFirst = std::max(first, sourceCount);
    if (pendingFirst <= last) {
        emit rangeAvailable(pendingFirst, last);
    }

    if (!source || sourceCount <= 0 || first >= sourceCount) {
        emit rangeRequestFinished(requestedFirst, requestedLast);
        return;
    }

    const int sourceFirst = first;
    const int sourceLast = std::min(last, sourceCount - 1);
    pendingRequests.push_back(
        {sourceFirst, sourceLast, requestedFirst, requestedLast});
    source->requestRange(sourceFirst, sourceLast, reason, generation);
}

bool OutboxPostSource::canRequestBeforeFirst() const
{
    return source && source->canRequestBeforeFirst();
}

void OutboxPostSource::requestBeforeFirst(RequestReason reason,
                                          quint64 generation)
{
    if (source) {
        source->requestBeforeFirst(reason, generation);
    }
}

bool OutboxPostSource::isPendingPostId(const QString& postId) const
{
    return pendingIds.contains(postId);
}

const PendingPost* OutboxPostSource::pendingPostAt(int index) const
{
    if (!isPendingIndex(index)) {
        return nullptr;
    }
    return outbox.pendingPost(pendingIds.at(index - sourceCount));
}

const PendingPost* OutboxPostSource::pendingPost(const QString& postId) const
{
    return isPendingPostId(postId) ? outbox.pendingPost(postId) : nullptr;
}

std::shared_ptr<BackendPost> OutboxPostSource::pendingSnapshotAt(int index) const
{
    if (!isPendingIndex(index)) {
        return {};
    }
    return outbox.snapshotLease(pendingIds.at(index - sourceCount));
}

void OutboxPostSource::handleSourceItemCountChanged(int count)
{
    count = std::max(0, count);
    if (count == sourceCount) {
        return;
    }

    if (!pendingIds.isEmpty()) {
        scheduleSourceStructureSync();
        return;
    }

    sourceCount = count;
    emit itemCountChanged(itemCount());
}

void OutboxPostSource::scheduleSourceStructureSync()
{
    if (sourceStructureSyncScheduled) {
        return;
    }
    sourceStructureSyncScheduled = true;
    QPointer<OutboxPostSource> guard(this);
    QTimer::singleShot(0, this, [guard] {
        if (guard) {
            guard->syncSourceStructure();
        }
    });
}

void OutboxPostSource::syncSourceStructure()
{
    sourceStructureSyncScheduled = false;
    if (!source) {
        return;
    }

    const int nextSourceCount = std::max(0, source->itemCount());
    if (nextSourceCount == sourceCount) {
        return;
    }

    const int previous = sourceCount;
    sourceCount = nextSourceCount;
    if (nextSourceCount > previous) {
        emit itemsInserted(previous, nextSourceCount - previous);
        emitSourceAvailableRuns(previous, nextSourceCount - 1);
    } else {
        emit itemsRemoved(nextSourceCount, previous - nextSourceCount);
    }
}

void OutboxPostSource::emitSourceAvailableRuns(int first, int last)
{
    if (!source || sourceCount <= 0) {
        return;
    }

    first = std::max(0, first);
    last = std::min(sourceCount - 1, last);
    int runFirst = -1;
    for (int index = first; index <= last; ++index) {
        if (source->isAvailable(index)) {
            if (runFirst < 0) {
                runFirst = index;
            }
            continue;
        }
        if (runFirst >= 0) {
            emit rangeAvailable(runFirst, index - 1);
            runFirst = -1;
        }
    }
    if (runFirst >= 0) {
        emit rangeAvailable(runFirst, last);
    }
}

bool OutboxPostSource::sourceBoundaryMatchesPendingHead() const
{
    if (!source || pendingIds.isEmpty()
        || source->itemCount() <= sourceCount) {
        return false;
    }

    BackendPost* post = source->postAt(sourceCount);
    return post && !post->pending_post_id.isEmpty()
        && post->pending_post_id == pendingIds.first();
}

void OutboxPostSource::handlePendingConfirmed(
    const QString& pendingPostId,
    const QString& serverPostId)
{
    if (pendingIds.contains(pendingPostId) && !serverPostId.isEmpty()) {
        confirmedServerIds.insert(pendingPostId, serverPostId);
    }
}

bool OutboxPostSource::tryPromotePending(
    const QString& pendingPostId,
    const QString& serverPostId)
{
    if (!source || pendingIds.isEmpty()
        || pendingIds.first() != pendingPostId) {
        return false;
    }

    const int nextSourceCount = std::max(0, source->itemCount());
    const int serverIndex = source->indexOfPost(serverPostId);
    if (nextSourceCount != sourceCount + 1
        || serverIndex != sourceCount
        || !source->isAvailable(serverIndex)) {
        return false;
    }

    const int index = sourceCount;
    sourceCount = nextSourceCount;
    pendingIds.removeFirst();
    sourceStructureSyncScheduled = false;
    emit pendingPromoted(index, pendingPostId, serverPostId);
    return true;
}

bool OutboxPostSource::matchesConversation(
    const QString& changedChannelId,
    const QString& changedRootId) const
{
    return changedChannelId == channelId && changedRootId == rootId;
}

void OutboxPostSource::handlePendingAdded(
    const QString& changedChannelId,
    const QString& changedRootId,
    const QString& pendingPostId)
{
    if (!matchesConversation(changedChannelId, changedRootId)
        || pendingPostId.isEmpty() || pendingIds.contains(pendingPostId)) {
        return;
    }

    const int index = sourceCount + pendingIds.size();
    pendingIds.push_back(pendingPostId);
    emit itemsInserted(index, 1);
    emit rangeAvailable(index, index);
}

void OutboxPostSource::handlePendingChanged(
    const QString& changedChannelId,
    const QString& changedRootId,
    const QString& pendingPostId)
{
    if (!matchesConversation(changedChannelId, changedRootId)) {
        return;
    }
    const int pendingIndex = pendingIds.indexOf(pendingPostId);
    if (pendingIndex < 0) {
        return;
    }
    // Delivery state changes presentation only. They must never enter the
    // structural remap path: the row identity and logical index are unchanged.
    emit pendingPresentationChanged(pendingPostId);
}

void OutboxPostSource::handlePendingRemoved(
    const QString& changedChannelId,
    const QString& changedRootId,
    const QString& pendingPostId)
{
    if (!matchesConversation(changedChannelId, changedRootId)) {
        return;
    }

    const QString serverPostId = confirmedServerIds.take(pendingPostId);
    if (!serverPostId.isEmpty()
        && tryPromotePending(pendingPostId, serverPostId)) {
        return;
    }

    syncSourceStructure();
    const int pendingIndex = pendingIds.indexOf(pendingPostId);
    if (pendingIndex < 0) {
        return;
    }
    pendingIds.removeAt(pendingIndex);
    emit itemsRemoved(sourceCount + pendingIndex, 1);
}

} // namespace Mattermost
