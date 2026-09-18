#include "FilteredPostSource.h"

#include <algorithm>
#include <utility>

#include "backend/types/BackendPost.h"

namespace Mattermost {

FilteredPostSource::FilteredPostSource(AbstractPostSource& sourceInstance,
                                       Predicate predicateInstance,
                                       QObject* parent)
    : AbstractPostSource(parent)
    , source(&sourceInstance)
    , predicate(std::move(predicateInstance))
    , sourceCount(sourceInstance.itemCount())
{
    // The wrapped source may already have a small resident cache (for example
    // ChannelPostSource's newest seed) before this proxy is constructed.
    evaluateRange(0, sourceCount - 1);
    rejectedSourceIndices = currentRejectedSourceIndices();

    connect(&sourceInstance, &AbstractPostSource::itemCountChanged,
            this, &FilteredPostSource::handleItemCountChanged);
    connect(&sourceInstance, &AbstractPostSource::itemsInserted,
            this, &FilteredPostSource::handleItemsInserted);
    connect(&sourceInstance, &AbstractPostSource::itemsRemoved,
            this, &FilteredPostSource::handleItemsRemoved);
    connect(&sourceInstance, &AbstractPostSource::rangeAvailable,
            this, &FilteredPostSource::handleRangeAvailable);
    connect(&sourceInstance, &AbstractPostSource::bodyAvailabilityChanged,
            this, &FilteredPostSource::handleBodyAvailabilityChanged);
    connect(&sourceInstance, &AbstractPostSource::layoutChanged,
            this, &FilteredPostSource::handleLayoutChanged);
    connect(&sourceInstance, &AbstractPostSource::seekTargetResolved,
            this, &FilteredPostSource::handleSeekTargetResolved);
    connect(&sourceInstance, &AbstractPostSource::rangeRequestFinished,
            this, &FilteredPostSource::handleRangeRequestFinished);
    connect(&sourceInstance, &QObject::destroyed, this, [this] {
        const int previousCount = itemCount();
        source.clear();
        sourceCount = 0;
        rejectedSourceIndices.clear();
        rejectedPostIds.clear();
        pendingRequests.clear();
        if (previousCount != 0) {
            emit itemCountChanged(0);
        }
    });
}

int FilteredPostSource::itemCount() const
{
    return std::max(0, sourceCount - static_cast<int>(rejectedSourceIndices.size()));
}

bool FilteredPostSource::isAvailable(int index) const
{
    if (!source) {
        return false;
    }
    const int sourceIndex = sourceIndexForFiltered(index);
    return sourceIndex >= 0 && source->isAvailable(sourceIndex);
}

BackendPost* FilteredPostSource::postAt(int index) const
{
    if (!source) {
        return nullptr;
    }
    const int sourceIndex = sourceIndexForFiltered(index);
    return sourceIndex >= 0 ? source->postAt(sourceIndex) : nullptr;
}

QString FilteredPostSource::postIdAt(int index) const
{
    if (!source) {
        return {};
    }
    const int sourceIndex = sourceIndexForFiltered(index);
    return sourceIndex >= 0 ? source->postIdAt(sourceIndex) : QString();
}

int FilteredPostSource::indexOfPost(const QString& postId) const
{
    if (!source || postId.isEmpty()) {
        return -1;
    }
    return filteredIndexForSource(source->indexOfPost(postId));
}

int FilteredPostSource::ensurePostIndex(const QString& postId)
{
    if (!source || postId.isEmpty()) {
        return -1;
    }

    const int sourceIndex = source->ensurePostIndex(postId);
    if (sourceIndex < 0) {
        return -1;
    }

    // ensurePostIndex() is allowed to resolve a cached target without another
    // rangeAvailable signal. Classify it before exposing the corresponding row.
    evaluateRange(sourceIndex, sourceIndex);
    applyRejectedSourceIndices(currentRejectedSourceIndices());
    return filteredIndexForSource(sourceIndex);
}

void FilteredPostSource::requestRange(int first,
                                      int last,
                                      RequestReason reason,
                                      quint64 generation)
{
    const int requestedFirst = first;
    const int requestedLast = last;

    if (!source || itemCount() <= 0) {
        emit rangeRequestFinished(requestedFirst, requestedLast);
        return;
    }

    first = std::max(0, first);
    last = std::min(itemCount() - 1, last);
    if (last < first) {
        emit rangeRequestFinished(requestedFirst, requestedLast);
        return;
    }

    const int sourceFirst = sourceIndexForFiltered(first);
    const int sourceLast = sourceIndexForFiltered(last);
    if (sourceFirst < 0 || sourceLast < sourceFirst) {
        emit rangeRequestFinished(requestedFirst, requestedLast);
        return;
    }

    // Store the filtered coordinates before entering the wrapped source: an
    // already-resident request is allowed to complete synchronously.
    pendingRequests.push_back(
        {sourceFirst, sourceLast, requestedFirst, requestedLast});
    source->requestRange(sourceFirst, sourceLast, reason, generation);
}

bool FilteredPostSource::canRequestBeforeFirst() const
{
    return source && source->canRequestBeforeFirst();
}

void FilteredPostSource::requestBeforeFirst(RequestReason reason, quint64 generation)
{
    if (source) {
        source->requestBeforeFirst(reason, generation);
    }
}

void FilteredPostSource::setPredicate(Predicate nextPredicate)
{
    predicate = std::move(nextPredicate);
    rejectedPostIds.clear();
    evaluateRange(0, sourceCount - 1);
    applyRejectedSourceIndices(currentRejectedSourceIndices());
}

void FilteredPostSource::invalidatePost(const QString& postId)
{
    if (!source || postId.isEmpty()) {
        return;
    }

    rejectedPostIds.remove(postId);
    const int sourceIndex = source->indexOfPost(postId);
    if (sourceIndex >= 0) {
        evaluateRange(sourceIndex, sourceIndex);
    }
    applyRejectedSourceIndices(currentRejectedSourceIndices());
}

void FilteredPostSource::evaluateRange(int first, int last)
{
    if (!source || sourceCount <= 0) {
        return;
    }

    first = std::max(0, first);
    last = std::min(sourceCount - 1, last);
    if (last < first) {
        return;
    }

    for (int sourceIndex = first; sourceIndex <= last; ++sourceIndex) {
        BackendPost* post = source->postAt(sourceIndex);
        if (!post) {
            continue;
        }

        QString postId = source->postIdAt(sourceIndex);
        if (postId.isEmpty()) {
            postId = post->id;
        }
        if (postId.isEmpty()) {
            continue;
        }

        const bool accepted = !predicate || predicate(*post);
        if (accepted) {
            rejectedPostIds.remove(postId);
        } else {
            rejectedPostIds.insert(postId);
        }
    }
}

QVector<int> FilteredPostSource::currentRejectedSourceIndices() const
{
    QVector<int> result;
    if (!source || rejectedPostIds.isEmpty()) {
        return result;
    }

    result.reserve(rejectedPostIds.size());
    for (const QString& postId : rejectedPostIds) {
        const int index = source->indexOfPost(postId);
        if (index >= 0 && index < sourceCount) {
            result.push_back(index);
        }
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void FilteredPostSource::applyRejectedSourceIndices(const QVector<int>& target)
{
    QVector<int> newlyRejected;
    QVector<int> newlyVisible;

    for (int sourceIndex : target) {
        if (!std::binary_search(rejectedSourceIndices.cbegin(),
                                rejectedSourceIndices.cend(),
                                sourceIndex)) {
            newlyRejected.push_back(sourceIndex);
        }
    }
    for (int sourceIndex : std::as_const(rejectedSourceIndices)) {
        if (!std::binary_search(target.cbegin(), target.cend(), sourceIndex)) {
            newlyVisible.push_back(sourceIndex);
        }
    }

    // Remove from the highest source coordinate first. This keeps every lower
    // filtered index stable while each signal is delivered.
    std::sort(newlyRejected.begin(), newlyRejected.end(), std::greater<int>());
    for (int sourceIndex : std::as_const(newlyRejected)) {
        const int filteredIndex = filteredIndexForSource(sourceIndex);
        auto it = std::lower_bound(rejectedSourceIndices.begin(),
                                   rejectedSourceIndices.end(),
                                   sourceIndex);
        rejectedSourceIndices.insert(
            static_cast<int>(std::distance(rejectedSourceIndices.begin(), it)),
            sourceIndex);
        if (filteredIndex >= 0) {
            emit itemsRemoved(filteredIndex, 1);
        }
    }

    // Reinsert from low to high for the symmetric reason.
    std::sort(newlyVisible.begin(), newlyVisible.end());
    for (int sourceIndex : std::as_const(newlyVisible)) {
        auto it = std::lower_bound(rejectedSourceIndices.begin(),
                                   rejectedSourceIndices.end(),
                                   sourceIndex);
        if (it == rejectedSourceIndices.end() || *it != sourceIndex) {
            continue;
        }
        rejectedSourceIndices.erase(it);
        emit itemsInserted(filteredInsertionIndexForSource(sourceIndex), 1);
    }

    // Defensive normalization in case a future producer delivers duplicated or
    // unexpectedly reordered source notifications.
    rejectedSourceIndices = target;
}

void FilteredPostSource::emitLayoutForSourceRange(int first, int last)
{
    if (itemCount() <= 0 || sourceCount <= 0) {
        return;
    }

    first = std::max(0, first);
    last = std::min(sourceCount - 1, last);
    int filteredFirst = -1;
    int filteredLast = -1;
    for (int sourceIndex = first; sourceIndex <= last; ++sourceIndex) {
        const int filteredIndex = filteredIndexForSource(sourceIndex);
        if (filteredIndex < 0) {
            continue;
        }
        if (filteredFirst < 0) {
            filteredFirst = filteredIndex;
        }
        filteredLast = filteredIndex;
    }

    if (filteredFirst >= 0) {
        emit layoutChanged(filteredFirst, filteredLast);
    }
}

void FilteredPostSource::handleItemCountChanged(int count)
{
    count = std::max(0, count);
    const int previousFilteredCount = itemCount();
    const int previousSourceCount = sourceCount;
    sourceCount = count;

    if (count > previousSourceCount) {
        evaluateRange(previousSourceCount, count - 1);
    }
    rejectedSourceIndices = currentRejectedSourceIndices();

    const int nextFilteredCount = itemCount();
    if (nextFilteredCount != previousFilteredCount) {
        emit itemCountChanged(nextFilteredCount);
    }
}

void FilteredPostSource::handleItemsInserted(int first, int count)
{
    first = std::max(0, std::min(first, sourceCount));
    count = std::max(0, count);
    if (count == 0) {
        return;
    }

    const int filteredFirst = filteredInsertionIndexForSource(first);
    sourceCount += count;

    evaluateRange(first, first + count - 1);
    const QVector<int> target = currentRejectedSourceIndices();
    const auto firstHidden = std::lower_bound(target.cbegin(), target.cend(), first);
    const auto afterHidden = std::lower_bound(target.cbegin(), target.cend(), first + count);
    const int hiddenInserted = static_cast<int>(std::distance(firstHidden, afterHidden));
    const int visibleInserted = count - hiddenInserted;

    rejectedSourceIndices = target;
    if (visibleInserted > 0) {
        emit itemsInserted(filteredFirst, visibleInserted);
        emitAvailableRuns(first, first + count - 1);
    }
}

void FilteredPostSource::handleItemsRemoved(int first, int count)
{
    first = std::max(0, std::min(first, sourceCount));
    count = std::max(0, std::min(count, sourceCount - first));
    if (count == 0) {
        return;
    }

    const int filteredFirst = filteredInsertionIndexForSource(first);
    const auto firstHidden = std::lower_bound(
        rejectedSourceIndices.cbegin(), rejectedSourceIndices.cend(), first);
    const auto afterHidden = std::lower_bound(
        rejectedSourceIndices.cbegin(), rejectedSourceIndices.cend(), first + count);
    const int hiddenRemoved = static_cast<int>(std::distance(firstHidden, afterHidden));
    const int visibleRemoved = count - hiddenRemoved;

    sourceCount -= count;
    rejectedSourceIndices = currentRejectedSourceIndices();

    if (visibleRemoved > 0) {
        emit itemsRemoved(filteredFirst, visibleRemoved);
    }
}

void FilteredPostSource::handleRangeAvailable(int first, int last)
{
    evaluateRange(first, last);
    applyRejectedSourceIndices(currentRejectedSourceIndices());
    emitAvailableRuns(first, last);
}

void FilteredPostSource::handleBodyAvailabilityChanged(int first,
                                                       int last,
                                                       bool available)
{
    if (available) {
        evaluateRange(first, last);
        applyRejectedSourceIndices(currentRejectedSourceIndices());
    }
    emitBodyAvailabilityRuns(first, last, available);
}

void FilteredPostSource::handleLayoutChanged(int first, int last)
{
    evaluateRange(first, last);
    applyRejectedSourceIndices(currentRejectedSourceIndices());
    emitLayoutForSourceRange(first, last);
}

void FilteredPostSource::handleSeekTargetResolved(int sourceIndex,
                                                  quint64 generation)
{
    evaluateRange(sourceIndex, sourceIndex);
    applyRejectedSourceIndices(currentRejectedSourceIndices());
    const int filteredIndex = filteredIndexForSource(sourceIndex);
    if (filteredIndex >= 0) {
        emit seekTargetResolved(filteredIndex, generation);
    }
}

void FilteredPostSource::handleRangeRequestFinished(int first, int last)
{
    for (int i = 0; i < pendingRequests.size(); ++i) {
        const PendingRequest request = pendingRequests.at(i);
        if (request.sourceFirst != first || request.sourceLast != last) {
            continue;
        }
        pendingRequests.removeAt(i);
        emit rangeRequestFinished(request.filteredFirst, request.filteredLast);
        return;
    }
}

void FilteredPostSource::emitAvailableRuns(int sourceFirst, int sourceLast)
{
    if (!source || sourceCount <= 0) {
        return;
    }

    sourceFirst = std::max(0, sourceFirst);
    sourceLast = std::min(sourceCount - 1, sourceLast);

    int runFirst = -1;
    int runLast = -1;
    for (int sourceIndex = sourceFirst; sourceIndex <= sourceLast; ++sourceIndex) {
        const int filteredIndex = filteredIndexForSource(sourceIndex);
        const bool available = filteredIndex >= 0 && source->isAvailable(sourceIndex);
        if (available) {
            if (runFirst < 0) {
                runFirst = filteredIndex;
                runLast = filteredIndex;
            } else if (filteredIndex == runLast + 1) {
                runLast = filteredIndex;
            } else {
                emit rangeAvailable(runFirst, runLast);
                runFirst = filteredIndex;
                runLast = filteredIndex;
            }
        } else if (runFirst >= 0) {
            emit rangeAvailable(runFirst, runLast);
            runFirst = -1;
            runLast = -1;
        }
    }
    if (runFirst >= 0) {
        emit rangeAvailable(runFirst, runLast);
    }
}

void FilteredPostSource::emitBodyAvailabilityRuns(int sourceFirst,
                                                  int sourceLast,
                                                  bool available)
{
    if (!source || sourceCount <= 0) {
        return;
    }

    sourceFirst = std::max(0, sourceFirst);
    sourceLast = std::min(sourceCount - 1, sourceLast);

    int runFirst = -1;
    int runLast = -1;
    for (int sourceIndex = sourceFirst; sourceIndex <= sourceLast; ++sourceIndex) {
        const int filteredIndex = filteredIndexForSource(sourceIndex);
        const bool applies = filteredIndex >= 0
            && (!available || source->isAvailable(sourceIndex));
        if (applies) {
            if (runFirst < 0) {
                runFirst = filteredIndex;
                runLast = filteredIndex;
            } else if (filteredIndex == runLast + 1) {
                runLast = filteredIndex;
            } else {
                emit bodyAvailabilityChanged(runFirst, runLast, available);
                runFirst = filteredIndex;
                runLast = filteredIndex;
            }
        } else if (runFirst >= 0) {
            emit bodyAvailabilityChanged(runFirst, runLast, available);
            runFirst = -1;
            runLast = -1;
        }
    }
    if (runFirst >= 0) {
        emit bodyAvailabilityChanged(runFirst, runLast, available);
    }
}

bool FilteredPostSource::isRejectedSourceIndex(int sourceIndex) const
{
    return std::binary_search(rejectedSourceIndices.cbegin(),
                              rejectedSourceIndices.cend(),
                              sourceIndex);
}

int FilteredPostSource::rejectedBefore(int sourceIndex) const
{
    return static_cast<int>(std::distance(
        rejectedSourceIndices.cbegin(),
        std::lower_bound(rejectedSourceIndices.cbegin(),
                         rejectedSourceIndices.cend(),
                         sourceIndex)));
}

int FilteredPostSource::filteredIndexForSource(int sourceIndex) const
{
    if (sourceIndex < 0 || sourceIndex >= sourceCount
        || isRejectedSourceIndex(sourceIndex)) {
        return -1;
    }
    return sourceIndex - rejectedBefore(sourceIndex);
}

int FilteredPostSource::sourceIndexForFiltered(int index) const
{
    if (index < 0 || index >= itemCount() || sourceCount <= 0) {
        return -1;
    }

    int low = 0;
    int high = sourceCount - 1;
    while (low < high) {
        const int middle = low + (high - low) / 2;
        const int rejectedThroughMiddle = static_cast<int>(std::distance(
            rejectedSourceIndices.cbegin(),
            std::upper_bound(rejectedSourceIndices.cbegin(),
                             rejectedSourceIndices.cend(),
                             middle)));
        const int visibleThroughMiddle =
            middle + 1 - rejectedThroughMiddle;
        if (visibleThroughMiddle > index) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }

    return isRejectedSourceIndex(low) ? -1 : low;
}

int FilteredPostSource::filteredInsertionIndexForSource(int sourceIndex) const
{
    sourceIndex = std::max(0, std::min(sourceIndex, sourceCount));
    return sourceIndex - rejectedBefore(sourceIndex);
}

} // namespace Mattermost
