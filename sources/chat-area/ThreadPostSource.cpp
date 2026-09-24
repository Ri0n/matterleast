#include "ThreadPostSource.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>

#include "ThreadTimelineSizing.h"
#include "backend/Backend.h"
#include "backend/PostTimelineService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

namespace {

Q_LOGGING_CATEGORY(lcThreadTimelineTrace, "mattermost.timeline.thread", QtWarningMsg)

QString shortId(const QString& id)
{
    return id.isEmpty() ? QStringLiteral("-") : id.left(8);
}

QString slotSummary(const QVector<QString>& postIds)
{
    int available = 0;
    for (const QString& id : postIds) {
        available += !id.isEmpty();
    }

    QString result = QStringLiteral("count=%1 known=%2 missing=%3")
        .arg(postIds.size())
        .arg(available)
        .arg(postIds.size() - available);
    if (postIds.size() > 50) {
        return result;
    }

    result += QStringLiteral(" slots=");
    for (int index = 0; index < postIds.size(); ++index) {
        if (index != 0) {
            result += QLatin1Char(' ');
        }
        result += QString::number(index);
        result += QLatin1Char(':');
        result += shortId(postIds.at(index));
    }
    return result;
}

QString idsSummary(const QStringList& ids)
{
    QStringList result;
    result.reserve(ids.size());
    for (const QString& id : ids) {
        result.push_back(shortId(id));
    }
    return result.join(QLatin1Char(','));
}

const char* requestReasonName(AbstractPostSource::RequestReason reason)
{
    switch (reason) {
    case AbstractPostSource::RequestReason::Initial:
        return "initial";
    case AbstractPostSource::RequestReason::Scroll:
        return "scroll";
    case AbstractPostSource::RequestReason::Seek:
        return "seek";
    case AbstractPostSource::RequestReason::EnsureVisible:
        return "ensure-visible";
    }
    return "unknown";
}

QVector<BackendPost*> cachedThreadReplies(const BackendChannel& channel, const QString& rootId)
{
    QVector<BackendPost*> result;
    for (const BackendPost& post : channel.posts) {
        // BackendChannel marks replies hidden so the main channel renders only
        // root posts. That flag is expected on thread replies and must not hide
        // them from the thread's own logical sequence.
        if (post.root_id == rootId
            && !(post.isDeleted || post.delete_at != 0)) {
            BackendPost* cached = channel.postIdToPost.value(post.id, nullptr);
            if (cached) {
                result.push_back(cached);
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const BackendPost* lhs, const BackendPost* rhs) {
        if (lhs->create_at != rhs->create_at) {
            return lhs->create_at < rhs->create_at;
        }
        return lhs->id < rhs->id;
    });
    return result;
}

} // namespace

ThreadPostSource::ThreadPostSource(Backend& backendInstance,
                                   BackendChannel& channelInstance,
                                   QString sourceRootId,
                                   QObject* parent)
    : IndexedPostSource(channelInstance, parent)
    , backend(backendInstance)
    , rootId(std::move(sourceRootId))
{
    postIds.resize(currentLogicalCount());
    if (!postIds.isEmpty()) {
        if (BackendPost* root = rootPost()) {
            postIds[0] = rootId;
            cursorCreateAtById.insert(rootId, root->create_at);
        }
    }
    seedCachedPosts();

    BackendPost* root = rootPost();
    if (root) {
        rootResidencyLease = PostTimelineService::instance(backend).leasePost(*root);
    }
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_INIT source=" << static_cast<const void*>(this)
        << " root=" << shortId(rootId)
        << " replyCount=" << (root ? root->reply_count : -1)
        << " lastReplyAt=" << (root ? root->last_reply_at : 0)
        << ' ' << slotSummary(postIds);

    const auto syncLogicalCount = [this](const BackendPost& rootPost) {
        if (rootPost.id != rootId) {
            return;
        }
        const int count = currentLogicalCount();
        if (count == static_cast<int>(postIds.size())) {
            return;
        }

        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_COUNT_CHANGE source=" << static_cast<const void*>(this)
            << " old=" << postIds.size()
            << " new=" << count
            << " replyCount=" << rootPost.reply_count;
        resizeLogicalTail(count);
        pruneProvisionalPostIds();
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_SLOTS source=" << static_cast<const void*>(this)
            << ' ' << slotSummary(postIds);
    };

    connect(&channel, &BackendChannel::onNewPost, this,
            [this](BackendPost& post) { appendLiveReply(post); });
    connect(&channel, &BackendChannel::onPostEdited, this,
            [this, syncLogicalCount](BackendPost& post) {
        const int index = indexOfPost(post.id);
        if (index >= 0) {
            qCDebug(lcThreadTimelineTrace).nospace()
                << "THREAD_POST_EDIT source=" << static_cast<const void*>(this)
                << " post=" << shortId(post.id)
                << " index=" << index;
        }
        syncLogicalCount(post);
    });
    connect(&channel, &BackendChannel::onThreadSummaryChanged, this,
            [syncLogicalCount](BackendPost& rootPost) {
        // Reply-count/thread-summary changes alter the logical thread topology,
        // but not the root post body. Keep the source count current without
        // publishing itemsChanged(0), which would rematerialize the thread root.
        syncLogicalCount(rootPost);
    });
    connect(&channel, &BackendChannel::onPostDeleted, this,
            [this](const QString& postId) {
        const int index = indexOfPost(postId);
        if (index >= 0) {
            retainMappedTombstone(postId);
        }
    });

    QTimer::singleShot(0, this, [this] { hydrateCachedTail(); });
}

bool ThreadPostSource::isAvailable(int index) const
{
    if (!IndexedPostSource::isAvailable(index)) {
        return false;
    }

    // A body fetched for permalink navigation does not make its timestamp-based
    // estimated slot a renderable row. Keep only the semantic identity/index for
    // scrolling and range selection; materialize after a server window confirms
    // or relocates the same post.
    return !navigationPlacement.blocksMaterialization(postIds.at(index));
}

int ThreadPostSource::ensurePostIndex(const QString& postId)
{
    const int existing = indexOfPost(postId);
    if (existing >= 0) {
        if (provisionalPostIds.contains(postId)) {
            // Cached thread windows may already expose a provisional row. Keep
            // that row renderable, but protect its semantic identity until a
            // server window confirms or relocates it.
            navigationPlacement.trackExistingProvisional(postId, existing);
            if (!islandForPost(postId)) {
                islands.push_back(Island {postId, QStringList {postId}, existing, false, false, false});
            }
        }
        return existing;
    }

    BackendPost* post = channel.postIdToPost.value(postId, nullptr);
    if (!post || postIds.isEmpty()
        || (post->id != rootId && post->root_id != rootId)) {
        return -1;
    }
    cursorCreateAtById.insert(post->id, post->create_at);
    if (post->id == rootId) {
        postIds[0] = rootId;
        rebuildIndex();
        navigationPlacement.clear();
        emit rangeAvailable(0, 0);
        return 0;
    }

    const int index = nearestEmptyIndex(estimatedIndexForPost(*post));
    if (index < 1) {
        return -1;
    }
    postIds[index] = postId;
    provisionalPostIds.insert(postId);
    navigationPlacement.trackEstimated(postId, index);
    ++islandEpoch;
    islands.push_back(Island {postId, QStringList {postId}, index, false, false, false});
    rebuildIndex();
    pruneProvisionalPostIds();
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PROVISIONAL source=" << static_cast<const void*>(this)
        << " post=" << shortId(postId)
        << " index=" << index
        << " materializable=false"
        << ' ' << slotSummary(postIds);

    // Deliberately do not emit rangeAvailable(). LongListWidget can navigate to
    // this logical estimate while it remains unavailable and will request the
    // surrounding server window without constructing a temporary PostWidget.
    return index;
}

struct ThreadPostSource::Demand
{
    int first;
    int last;
    quint64 generation;
    QString navigationTarget;
    QVector<QPair<int, int>> requests;
    int expectedFirst = -1;
    int expectedLast = -1;
    bool finished = false;
    bool approximateSeek = false;
    QSet<QString> cursors;
    std::vector<PostResidencyLease> leases;
};

struct ThreadPostSource::IslandLoad
{
    std::vector<std::shared_ptr<Demand>> waiters;
    PostTimelineService::Page before;
    PostTimelineService::Page after;
    QString targetId;
    QString anchorId;
    quint64 epoch = 0;
    quint64 generation = 0;
    int pending = 0;
    bool bootstrap = false;
    bool backward = false;
};

const ThreadPostSource::Island* ThreadPostSource::islandForPost(const QString& postId) const
{
    for (const auto& candidate : islands) {
        if (candidate.ids.contains(postId)) return &candidate;
    }
    return nullptr;
}

bool ThreadPostSource::isPostReadyForNavigation(const QString& postId) const
{
    const auto* context = islandForPost(postId);
    return isAvailable(indexOfPost(postId))
        && (isPostPositionAuthoritative(postId) || (context && context->ready));
}

bool ThreadPostSource::placeIsland(Island next, int exactFirst, int reservedFirst, int reservedLast)
{
    // Merge only shared identities, never numeric contact. An overlap fixes the
    // relative origin of both cursor-connected windows, including unseen edges.
    QSet<QString> mergedTargets;
    bool grew;
    do {
        grew = false;
        for (const auto& existing : islands) {
            if (mergedTargets.contains(existing.targetId)) continue;
            int relative = 0;
            bool overlaps = false;
            for (int offset = 0; offset < existing.ids.size(); ++offset) {
                const int match = static_cast<int>(next.ids.indexOf(existing.ids.at(offset)));
                if (match < 0) continue;
                if (overlaps && relative != match - offset) return false;
                relative = match - offset;
                overlaps = true;
            }
            if (!overlaps) continue;
            const int start = std::min(0, relative);
            const int end = std::max(static_cast<int>(next.ids.size()), relative + static_cast<int>(existing.ids.size()));
            QStringList combined;
            for (int index = start; index < end; ++index) {
                const QString a = next.ids.value(index);
                const QString b = existing.ids.value(index - relative);
                if (!a.isEmpty() && !b.isEmpty() && a != b) return false;
                combined.append(a.isEmpty() ? b : a);
            }
            next.ids = combined;
            next.first += start;
            if (exactFirst >= 0) exactFirst += start;
            next.reachedOldest |= existing.reachedOldest;
            next.reachedNewest |= existing.reachedNewest;
            mergedTargets.insert(existing.targetId);
            grew = true;
        }
    } while (grew);
    if (next.ids.isEmpty() || !next.ids.contains(next.targetId)) return false;
    const int count = static_cast<int>(next.ids.size());
    const auto acceptOrigin = [&](int origin) {
        if (origin < 1 || origin + count > itemCount()) return false;
        if (exactFirst >= 0 && exactFirst != origin) return false;
        exactFirst = origin;
        return true;
    };
    if (exactFirst >= 0 && !acceptOrigin(exactFirst)) return false;
    if (count == itemCount() - 1 && !acceptOrigin(1)) return false;
    if (next.reachedOldest && !acceptOrigin(1)) return false;
    if (next.reachedNewest && !acceptOrigin(itemCount() - count)) return false;
    QSet<QString> unique;
    for (int offset = 0; offset < count; ++offset) {
        const QString& id = next.ids.at(offset);
        if (id.isEmpty() || unique.contains(id)) return false;
        unique.insert(id);
        const int known = indexOfPost(id);
        if (isAuthoritativeIndex(known) && !acceptOrigin(known - offset)) return false;
    }

    if (exactFirst >= 0) {
        const auto previousIslands = islands;
        for (const auto& other : previousIslands) {
            if (mergedTargets.contains(other.targetId)) continue;
            if (other.first <= exactFirst + count && other.last() + 1 >= exactFirst) {
                if (!placeIsland(other, -1, exactFirst, exactFirst + count - 1)) return false;
            }
        }
    }

    QVector<QString> nextMapping = postIds;
    for (const auto& existing : islands) {
        if (!mergedTargets.contains(existing.targetId)) continue;
        for (const QString& id : existing.ids) {
            const int index = indexOfPost(id);
            if (index >= 0 && provisionalPostIds.contains(id)) nextMapping[index].clear();
        }
    }
    int first = exactFirst;
    if (first < 0) {
        // Leave an unknown slot on both sides until identity overlap or a
        // server boundary proves adjacency. Account for an incoming exact page
        // before choosing a new location for a numerically colliding island.
        const auto freeWindow = [&](int start) {
            if (start < 2 || start + count >= itemCount()) return false;
            for (int index = start - 1; index <= start + count; ++index) {
                if (!nextMapping.at(index).isEmpty()
                    || (reservedFirst >= 0 && index >= reservedFirst && index <= reservedLast)) return false;
            }
            return true;
        };
        const int preferred = std::clamp(next.first, 1, std::max(1, itemCount() - count));
        for (int distance = 0; distance < itemCount(); ++distance) {
            if (freeWindow(preferred - distance)) { first = preferred - distance; break; }
            if (freeWindow(preferred + distance)) { first = preferred + distance; break; }
        }
        if (first < 0) return false;
    } else {
        for (int offset = 0; offset < count; ++offset) {
            const QString& existing = nextMapping.at(first + offset);
            if (!existing.isEmpty() && existing != next.ids.at(offset)
                && !provisionalPostIds.contains(existing)) return false;
        }
    }

    for (int index = 0; index < nextMapping.size(); ++index) {
        if (unique.contains(nextMapping.at(index))) nextMapping[index].clear();
    }
    for (int offset = 0; offset < count; ++offset) nextMapping[first + offset] = next.ids.at(offset);
    for (const QString& id : next.ids) {
        if (exactFirst >= 0) provisionalPostIds.remove(id);
        else provisionalPostIds.insert(id);
    }
    next.first = first;
    postIds = std::move(nextMapping);
    rebuildIndex();
    for (auto it = islands.begin(); it != islands.end();) {
        if (mergedTargets.contains(it->targetId)) it = islands.erase(it);
        else ++it;
    }
    if (exactFirst < 0) islands.push_back(next);
    if (next.ids.contains(navigationPlacement.postId())) {
        const QString target = navigationPlacement.postId();
        navigationPlacement.clear();
        if (exactFirst < 0) {
            if (next.ready) navigationPlacement.trackExistingProvisional(target, indexOfPost(target));
            else navigationPlacement.trackEstimated(target, indexOfPost(target));
        }
    }
    pruneProvisionalPostIds();
    mappingChanged(0, itemCount() - 1);
    if (exactFirst >= 0 || next.ready) emit rangeAvailable(first, first + count - 1);
    return true;
}

void ThreadPostSource::continueIslandDemand(const std::shared_ptr<Demand>& demand)
{
    const int targetIndex = indexOfPost(demand->navigationTarget);
    if (isAuthoritativeIndex(targetIndex)) {
        demand->navigationTarget.clear();
        demand->first = std::max(0, targetIndex - 2);
        demand->last = std::min(itemCount() - 1, targetIndex + 2);
        continueDemand(demand);
        return;
    }
    const auto* context = islandForPost(demand->navigationTarget);
    if (!context) {
        finishDemand(demand); // superseded semantic navigation
        return;
    }
    const Island island = *context;
    if (islandLoad && islandLoad->epoch == islandEpoch
        && islandLoad->targetId == island.targetId
        && islandLoad->generation == latestGeneration) {
        islandLoad->waiters.push_back(demand);
        return;
    }
    auto load = std::make_shared<IslandLoad>();
    load->targetId = island.targetId;
    load->epoch = islandEpoch;
    load->generation = latestGeneration;
    load->bootstrap = !island.ready;
    load->waiters.push_back(demand);
    int fetchCount = 15;
    if (load->bootstrap) {
        load->anchorId = island.targetId;
        load->pending = 2;
    } else {
        load->pending = 1;
        if (demand->first < island.first) {
            load->backward = true;
            load->anchorId = island.ids.first();
            fetchCount = std::clamp(island.first - demand->first, 10, 30);
        } else if (demand->last > island.last()) {
            load->anchorId = island.ids.last();
            fetchCount = std::clamp(demand->last - island.last(), 10, 30);
        } else {
            int missing = std::max(demand->first, island.first);
            while (missing <= demand->last && isAvailable(missing)) ++missing;
            if (missing > demand->last) { finishDemand(demand); return; }
            const int offset = missing - island.first;
            load->backward = offset == 0;
            load->anchorId = island.ids.value(load->backward ? 1 : offset - 1);
            fetchCount = 10;
        }
    }
    const uint64_t anchorTime = cursorCreateAtById.value(load->anchorId);
    if (load->anchorId.isEmpty() || !anchorTime) {
        failDemand(demand, QStringLiteral("Navigation island has no semantic cursor"));
        return;
    }
    if (BackendPost* target = channel.postIdToPost.value(island.targetId, nullptr)) {
        demand->leases.push_back(PostTimelineService::instance(backend).leasePost(*target));
    }
    islandLoad = load;
    QPointer<ThreadPostSource> guard(this);
    const auto acceptPage = [guard, load, anchorTime](bool backward, const PostTimelineService::Page& page) {
        if (!guard) return;
        (backward ? load->before : load->after) = page;
        if (--load->pending) return;
        if (guard->islandLoad == load) guard->islandLoad.reset();
        const auto finish = [&](const QString& error = {}) {
            for (const auto& waiter : load->waiters) {
                if (error.isEmpty()) guard->finishDemand(waiter);
                else guard->failDemand(waiter, error);
            }
        };
        if (load->epoch != guard->islandEpoch || load->generation != guard->latestGeneration
            || !guard->islandForPost(load->targetId)) { finish(); return; }
        const auto validPage = [&](const PostTimelineService::Page& result, bool before) {
            if (!result.success) return false;
            QString previous;
            uint64_t previousTime = 0;
            for (const QString& id : result.postIds) {
                const uint64_t time = result.createAtById.value(id);
                if (!time || (before ? !(time < anchorTime || (time == anchorTime && id < load->anchorId))
                                     : !(time > anchorTime || (time == anchorTime && id > load->anchorId)))) return false;
                if (!previous.isEmpty() && !(previousTime < time || (previousTime == time && previous < id))) return false;
                previous = id;
                previousTime = time;
            }
            return !result.postIds.isEmpty() || load->bootstrap || (result.hasNextKnown && !result.hasNext);
        };
        if (((load->bootstrap || load->backward) && !validPage(load->before, true))
            || ((load->bootstrap || !load->backward) && !validPage(load->after, false))) {
            finish(QStringLiteral("Navigation island page failed or made no semantic progress"));
            return;
        }
        if (load->bootstrap && load->before.postIds.isEmpty() && load->after.postIds.isEmpty()
            && guard->itemCount() > 2) {
            finish(QStringLiteral("Navigation context returned no replies around a known target"));
            return;
        }
        Island next = *guard->islandForPost(load->targetId);
        const int preferredTarget = guard->indexOfPost(next.targetId);
        guard->rememberCursorCreateAt(load->before.createAtById);
        guard->rememberCursorCreateAt(load->after.createAtById);
        QStringList ids = next.ids;
        ids.append(load->before.postIds);
        ids.append(load->after.postIds);
        ids.removeDuplicates();
        std::sort(ids.begin(), ids.end(), [&](const QString& a, const QString& b) {
            const auto ta = guard->cursorCreateAtById.value(a);
            const auto tb = guard->cursorCreateAtById.value(b);
            return ta == tb ? a < b : ta < tb;
        });
        next.ids = ids;
        next.first = preferredTarget - static_cast<int>(ids.indexOf(next.targetId));
        next.ready = true;
        next.reachedOldest |= load->before.success && load->before.hasNextKnown && !load->before.hasNext;
        next.reachedNewest |= load->after.success && load->after.hasNextKnown && !load->after.hasNext;
        if (!guard->placeIsland(std::move(next))) {
            finish(QStringLiteral("Cannot place navigation island without false adjacency"));
            return;
        }
        finish();
    };
    auto& repository = PostTimelineService::instance(backend);
    if (load->bootstrap || load->backward) {
        repository.loadThreadBefore(channel, rootId, load->anchorId, anchorTime, fetchCount,
            [acceptPage](const PostTimelineService::Page& page) { acceptPage(true, page); });
    }
    if (load->bootstrap || !load->backward) {
        repository.loadThreadAfter(channel, rootId, load->anchorId, anchorTime, fetchCount,
            [acceptPage](const PostTimelineService::Page& page) { acceptPage(false, page); });
    }
}

void ThreadPostSource::requestRange(int first, int last, RequestReason reason, quint64 generation)
{
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_REQUEST source=" << static_cast<const void*>(this)
        << " requested=[" << first << ',' << last << ']'
        << " reason=" << requestReasonName(reason) << " generation=" << generation;
    latestGeneration = std::max(latestGeneration, generation);
    const Island* interestedIsland = nullptr;
    for (const auto& candidate : islands) {
        if (first <= candidate.last() + 1 && last >= candidate.first - 1
            && !(first < candidate.first - 1 && isCursorReadyIndex(first - 1))) {
            interestedIsland = &candidate;
            break;
        }
    }
    const bool islandInterest = interestedIsland != nullptr;
    const bool canMerge = reason != RequestReason::Seek && !islandInterest
        && !(reason == RequestReason::EnsureVisible && navigationPlacement.isActive());
    for (auto it = activeDemands.begin(); it != activeDemands.end();) {
        auto active = it->lock();
        if (!active || active->finished) {
            it = activeDemands.erase(it);
            continue;
        }
        ++it;
        if (canMerge && first <= last && active->generation == latestGeneration
            && active->navigationTarget.isEmpty() && active->expectedFirst >= 0
            && first <= active->expectedLast + 1 && last >= active->expectedFirst - 1) {
            active->requests.push_back(qMakePair(first, last));
            active->first = std::min(active->first, first);
            active->last = std::max(active->last, last);
            qCDebug(lcThreadTimelineTrace) << "THREAD_DEMAND_ATTACH" << first << last
                                         << "expected" << active->expectedFirst << active->expectedLast;
            return;
        }
    }
    auto demand = std::make_shared<Demand>();
    demand->first = first;
    demand->last = last;
    demand->generation = latestGeneration;
    demand->requests.push_back(qMakePair(first, last));
    activeDemands.push_back(demand);
    if (islandInterest) {
        demand->navigationTarget = interestedIsland->targetId;
    } else if (reason == RequestReason::EnsureVisible && navigationPlacement.isActive()) {
        demand->navigationTarget = navigationPlacement.postId();
    }
    demand->approximateSeek = reason == RequestReason::Seek;
    continueDemand(demand);
}

void ThreadPostSource::seedSeekIsland(const std::shared_ptr<Demand>& demand)
{
    const int target = std::clamp((demand->first + demand->last) / 2, 1, itemCount() - 1);
    std::vector<ThreadSeekAnchor> anchors;
    for (int index = 1; index < itemCount(); ++index) {
        if (isCursorReadyIndex(index)) anchors.push_back({index, cursorCreateAt(index)});
    }
    const auto* root = rootPost();
    const auto estimate = threadSeekEstimate(target, itemCount() - 1,
        root->create_at, std::max(root->create_at, root->last_reply_at), anchors);
    const quint64 epoch = islandEpoch;
    QPointer<ThreadPostSource> guard(this);
    auto callback = [guard, demand, target, epoch](const PostTimelineService::Page& page) {
        if (!guard) return;
        if (demand->generation != guard->latestGeneration || epoch != guard->islandEpoch) {
            guard->finishDemand(demand);
            return;
        }
        if (!page.success || page.postIds.isEmpty()) {
            guard->failDemand(demand, QStringLiteral("Timestamp seek returned no usable thread context"));
            return;
        }
        guard->rememberCursorCreateAt(page.createAtById);
        const int middle = static_cast<int>(page.postIds.size()) / 2;
        Island next {page.postIds.at(middle), page.postIds, target - middle,
                     true, false, page.hasNextKnown && !page.hasNext};
        const QString targetId = next.targetId;
        for (const auto& id : page.postIds) {
            if (auto* body = guard->channel.postIdToPost.value(id, nullptr))
                demand->leases.push_back(PostTimelineService::instance(guard->backend).leasePost(*body));
        }
        if (!guard->placeIsland(std::move(next))) {
            guard->failDemand(demand, QStringLiteral("Cannot place timestamp island without false adjacency"));
            return;
        }
        // A cold seek has no visible identity to preserve yet. Its initial page
        // can already overlap proven data and land far from the requested rank.
        emit guard->seekTargetResolved(guard->indexOfPost(targetId), demand->generation);
        guard->finishDemand(demand);
    };
    PostTimelineService::instance(backend).loadThreadFromTime(
        channel, rootId, 30, estimate.createAt, std::move(callback));
}

void ThreadPostSource::continueDemand(const std::shared_ptr<Demand>& demand)
{
    if (demand->finished) return;
    const auto finish = [this, demand] { finishDemand(demand); };
    if (demand->generation < latestGeneration) {
        finish();
        return;
    }
    int first = std::max(0, demand->first);
    int last = std::min(itemCount() - 1, demand->last);
    if (!demand->navigationTarget.isEmpty()) {
        continueIslandDemand(demand);
        return;
    }
    if (first > last) {
        finish();
        return;
    }

    // Find one missing run. Its nearest *authoritative* neighbour need not be
    // numerically adjacent: cursor pages count the intervening replies exactly.
    int missingFirst = first;
    while (missingFirst <= last && isAvailable(missingFirst)
           && isAuthoritativeIndex(missingFirst)) ++missingFirst;
    if (missingFirst > last) {
        finish();
        return;
    }
    if (!rootPost()) {
        failDemand(demand, QStringLiteral("Thread root body unavailable"));
        return;
    }
    int missingLast = missingFirst;
    while (missingLast < last && (!isAvailable(missingLast + 1)
                                 || !isAuthoritativeIndex(missingLast + 1))) ++missingLast;
    int left = missingFirst - 1;
    while (left >= 0 && !isCursorReadyIndex(left)) --left;
    int right = missingLast + 1;
    while (right < itemCount() && !isCursorReadyIndex(right)) ++right;

    if (demand->approximateSeek
        && std::min(missingFirst - left, right - missingLast) > 30) {
        seedSeekIsland(demand);
        return;
    }

    // The newest source boundary is another exact anchor. Fetch it only when
    // closer than a known cursor; timestamp estimates never acquire numeric slots.
    const bool backward = left < 0 || right - missingLast < missingFirst - left;
    const bool tail = backward && right == itemCount();
    const int anchorIndex = backward ? right : left;
    const QString anchorId = tail ? QString() : postIds.value(anchorIndex);
    const uint64_t anchorTime = tail ? 0 : cursorCreateAt(anchorIndex);
    const int distance = backward ? anchorIndex - missingFirst : missingLast - anchorIndex;
    const int fetchCount = std::min(50, std::max(ServerBlockSize, distance));
    const QString key = (backward ? QStringLiteral("up:") : QStringLiteral("down:")) + anchorId;
    if (demand->cursors.contains(key)) {
        failDemand(demand, QStringLiteral("Cursor page did not satisfy or advance thread demand"));
        return;
    }
    demand->cursors.insert(key);
    // Keep the physical page's expected window fixed while interest grows.
    // Uncovered attached rows are satisfied by the next exact cursor page.
    demand->expectedFirst = backward ? std::max(1, anchorIndex - fetchCount) : anchorIndex + 1;
    demand->expectedLast = backward ? anchorIndex - 1 : std::min(itemCount() - 1, anchorIndex + fetchCount);
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_SEEK_BRIDGE source=" << static_cast<const void*>(this)
        << " direction=" << (backward ? "before" : "after")
        << " anchorIndex=" << anchorIndex << " anchor=" << shortId(anchorId)
        << " requested=[" << first << ',' << last << "] perPage=" << fetchCount;

    QPointer<ThreadPostSource> guard(this);
    auto callback = [guard, demand, first, last, anchorIndex, anchorId, backward, tail](const PostTimelineService::Page& result) {
        if (!guard) return;
        if (demand->generation < guard->latestGeneration) {
            guard->finishDemand(demand);
            return;
        }
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_SEEK_BRIDGE_RESPONSE source=" << static_cast<const void*>(guard.data())
            << " success=" << result.success << " anchorIndex=" << anchorIndex
            << " direction=" << (backward ? "up" : "down") << " tail=" << tail
            << " itemCount=" << guard->itemCount()
            << " ids=" << idsSummary(result.postIds) << " hasNext=" << result.hasNext;
        if (!result.success) {
            guard->failDemand(demand, QStringLiteral("Thread page transport failed"));
            return;
        }
        if (!tail && (!guard->isAuthoritativeIndex(anchorIndex)
                      || guard->postIds.value(anchorIndex) != anchorId)) {
            guard->failDemand(demand, QStringLiteral("Thread cursor changed during retrieval"));
            return;
        }
        const int count = result.postIds.size();
        const int pageFirst = tail ? guard->itemCount() - count
                             : backward ? anchorIndex - count : anchorIndex + 1;
        // Pagination exhaustion is not a count proof. In particular an empty
        // response (or omitted has_next) must not erase already known replies.
        // Root-summary ingestion owns count updates; validate this page before
        // making any mapping mutation.
        if (count == 0 && std::max(0, demand->first) >= guard->itemCount()) {
            guard->continueDemand(demand);
            return;
        }
        if (count == 0 || pageFirst < 1 || pageFirst + count > guard->itemCount()) {
            guard->failDemand(demand, QStringLiteral("Thread page contradicts logical boundaries"));
            return;
        }
        // A cursor continuation may overlap known rows only at the same ranks.
        // Numeric contact cannot relocate an independently known identity.
        for (int offset = 0; offset < count; ++offset) {
            const QString& id = result.postIds.at(offset);
            const int existing = guard->indexOfPost(id);
            const int target = pageFirst + offset;
            if ((existing >= 0 && guard->isAuthoritativeIndex(existing) && existing != target)
                || (guard->isAuthoritativeIndex(target) && guard->postIds.at(target) != id)) {
                guard->failDemand(demand, QStringLiteral("Thread cursor conflicts with confirmed ranks"));
                return;
            }
        }
        guard->rememberCursorCreateAt(result.createAtById);
        // Keep demanded bodies alive across all constituent pages. Off-target
        // bodies remain evictable; their lightweight cursors are sufficient.
        for (int offset = 0; offset < count; ++offset) {
            const int target = pageFirst + offset;
            const int interestFirst = demand->navigationTarget.isEmpty() ? demand->first : first;
            const int interestLast = demand->navigationTarget.isEmpty() ? demand->last : last;
            if ((target < interestFirst || target > interestLast)
                || (!demand->navigationTarget.isEmpty()
                    && !guard->isPostPositionAuthoritative(demand->navigationTarget)
                    && result.postIds.at(offset) != demand->navigationTarget)) continue;
            if (BackendPost* post = guard->channel.postIdToPost.value(result.postIds.at(offset), nullptr)) {
                demand->leases.push_back(PostTimelineService::instance(guard->backend).leasePost(*post));
            }
        }
        if (!guard->placeExactWindow(pageFirst, result.postIds)) {
            guard->failDemand(demand, QStringLiteral("Cannot reconcile thread navigation island"));
            return;
        }
        // Body rehydration can leave identity mapping unchanged. Publish the
        // completed server window even then so LongList can materialize it.
        emit guard->rangeAvailable(pageFirst, pageFirst + count - 1);
        QTimer::singleShot(0, guard, [guard, demand] {
            if (guard) guard->continueDemand(demand);
        });
    };
    auto& repository = PostTimelineService::instance(backend);
    if (tail) {
        repository.loadThreadTail(channel, rootId, fetchCount, std::move(callback));
    } else if (backward) {
        repository.loadThreadBefore(channel, rootId, anchorId, anchorTime, fetchCount, std::move(callback));
    } else {
        repository.loadThreadAfter(channel, rootId, anchorId, anchorTime, fetchCount, std::move(callback));
    }
}

void ThreadPostSource::failDemand(const std::shared_ptr<Demand>& demand, const QString& error)
{
    qCWarning(lcThreadTimelineTrace).nospace()
        << "THREAD_REQUEST_FAILED requested=[" << demand->first << ',' << demand->last << "] " << error;
    finishDemand(demand, error);
}

void ThreadPostSource::finishDemand(const std::shared_ptr<Demand>& demand, const QString& error)
{
    if (demand->finished) return;
    demand->finished = true;
    // Mark finished before publishing: slots may synchronously request more.
    for (const auto& request : demand->requests) {
        if (!error.isEmpty()) emit rangeRequestFailed(request.first, request.second, error);
        emit rangeRequestFinished(request.first, request.second);
    }
}

BackendPost* ThreadPostSource::rootPost() const
{
    return channel.postIdToPost.value(rootId, nullptr);
}

int ThreadPostSource::currentLogicalCount() const
{
    BackendPost* root = rootPost();
    if (!root) {
        return 0;
    }

    // Mattermost reply_count excludes deleted replies, and /thread never
    // returns them. Count a tombstone only if this source already mapped that
    // semantic identity before deletion; a deleted body merely surviving in
    // BackendChannel cache has no ordinal provenance in a fresh source and must
    // not reserve an unfillable logical slot.
    int mappedDeletedReplyTombstones = 0;
    for (int index = 1; index < static_cast<int>(postIds.size()); ++index) {
        const QString& postId = postIds.at(index);
        if (postId.isEmpty()) {
            continue;
        }
        BackendPost* post = channel.postIdToPost.value(postId, nullptr);
        if (post && post->root_id == rootId
            && (post->isDeleted || post->delete_at != 0)) {
            ++mappedDeletedReplyTombstones;
        }
    }

    int count = threadLogicalItemCount(
        root->reply_count, mappedDeletedReplyTombstones);

    // reply_count is metadata, not authority to destroy an identity that this
    // source has already mapped. Preserve the furthest confirmed row even if a
    // normal live body is later evicted from the residency cache.
    for (int index = static_cast<int>(postIds.size()) - 1; index >= count; --index) {
        if (!postIds.at(index).isEmpty() && !provisionalPostIds.contains(postIds.at(index))) {
            count = index + 1;
            break;
        }
    }
    return count;
}

void ThreadPostSource::retainMappedTombstone(const QString& postId)
{
    if (postId.isEmpty() || leasedTombstoneIds.contains(postId)
        || indexOfPost(postId) < 1) {
        return;
    }

    BackendPost* post = channel.postIdToPost.value(postId, nullptr);
    if (!post || post->root_id != rootId
        || !(post->isDeleted || post->delete_at != 0)) {
        return;
    }

    PostResidencyLease lease =
        PostTimelineService::instance(backend).leasePost(*post);
    if (!lease) {
        return;
    }

    leasedTombstoneIds.insert(postId);
    tombstoneResidencyLeases.push_back(std::move(lease));
}

int ThreadPostSource::nearestEmptyIndex(int preferred) const
{
    const int count = static_cast<int>(postIds.size());
    if (count <= 1) {
        return -1;
    }
    preferred = std::max(1, std::min(preferred, count - 1));
    if (postIds.at(preferred).isEmpty()) {
        return preferred;
    }
    for (int distance = 1; distance < count; ++distance) {
        const int before = preferred - distance;
        if (before >= 1 && postIds.at(before).isEmpty()) {
            return before;
        }
        const int after = preferred + distance;
        if (after < count && postIds.at(after).isEmpty()) {
            return after;
        }
    }
    return -1;
}

void ThreadPostSource::seedCachedPosts()
{
    BackendPost* root = rootPost();
    if (!root || postIds.isEmpty()) {
        return;
    }

    postIds[0] = rootId;
    const QVector<BackendPost*> replies = cachedThreadReplies(channel, rootId);
    for (BackendPost* reply : replies) {
        if (reply && reply->create_at != 0) {
            cursorCreateAtById.insert(reply->id, reply->create_at);
        }
    }
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_SEED source=" << static_cast<const void*>(this)
        << " root=" << shortId(rootId)
        << " replyCount=" << root->reply_count
        << " cachedReplies=" << replies.size();
    if (replies.isEmpty()) {
        rebuildIndex();
        emit rangeAvailable(0, 0);
        return;
    }

    if (postIds.size() - 1 == replies.size()) {
        for (int i = 0; i < replies.size(); ++i) {
            postIds[i + 1] = replies.at(i)->id;
        }
        rebuildIndex();
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_SEED_ALL source=" << static_cast<const void*>(this)
            << ' ' << slotSummary(postIds);
        emit rangeAvailable(0, static_cast<int>(postIds.size()) - 1);
        return;
    }

    // A partial BackendChannel cache has no contiguity/provenance metadata. It
    // may contain a tail page, an older context page, or both. Packing all such
    // replies into a suffix manufactures authoritative indices that we do not
    // actually know and later forces destructive remaps. Until the cache can
    // describe contiguous windows, only seed a partial thread at its real root.
    rebuildIndex();
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_SEED_ROOT_ONLY source=" << static_cast<const void*>(this)
        << " partialCachedReplies=" << replies.size()
        << ' ' << slotSummary(postIds);
    emit rangeAvailable(0, 0);
}

void ThreadPostSource::hydrateCachedTail()
{
    BackendPost* root = rootPost();
    if (!root || postIds.size() <= 1 || root->reply_count <= 0) {
        return;
    }

    PostTimelineService& repository = PostTimelineService::instance(backend);
    repository.recordChannelOpened(channel.id);
    QPointer<ThreadPostSource> guard(this);
    repository.loadCachedThreadTail(
        channel, rootId, ServerBlockSize,
        [guard](const PostTimelineService::Page& result) {
            if (!guard || !result.success || result.postIds.isEmpty()) {
                return;
            }
            guard->rememberCursorCreateAt(result.createAtById);
            BackendPost* root = guard->rootPost();
            BackendPost* newest = guard->channel.postIdToPost.value(
                result.postIds.last(), nullptr);
            if (!root || !newest || (root->last_reply_at != 0
                                     && newest->create_at != root->last_reply_at)) {
                qCDebug(lcThreadTimelineTrace).nospace()
                    << "THREAD_CACHE_TAIL_SKIP source="
                    << static_cast<const void*>(guard.data())
                    << " reason=newest-mismatch cached="
                    << (newest ? newest->create_at : 0)
                    << " root=" << (root ? root->last_reply_at : 0);
                return;
            }

            const int usableCount = std::min(
                static_cast<int>(result.postIds.size()),
                static_cast<int>(guard->postIds.size()) - 1);
            if (usableCount <= 0) {
                return;
            }
            const QStringList ids = result.postIds.mid(
                result.postIds.size() - usableCount);
            const int first = static_cast<int>(guard->postIds.size()) - usableCount;

            // Cached-tail hydration deliberately remains renderable while it is
            // validated, but it must not turn a freshly estimated permalink slot
            // into a concrete widget merely because the cache overlaps it. The
            // explicit navigation request will fetch the confirming server page.
            if (guard->navigationPlacement.isActive()
                && guard->navigationPlacement.blocksMaterialization(
                    guard->navigationPlacement.postId())) {
                const int estimate = guard->indexOfPost(
                    guard->navigationPlacement.postId());
                if (estimate >= first && estimate < first + usableCount) {
                    qCDebug(lcThreadTimelineTrace).nospace()
                        << "THREAD_CACHE_TAIL_SKIP source="
                        << static_cast<const void*>(guard.data())
                        << " reason=navigation-estimate target="
                        << shortId(guard->navigationPlacement.postId())
                        << " index=" << estimate;
                    return;
                }
            }

            for (int offset = 0; offset < usableCount; ++offset) {
                const int target = first + offset;
                const QString& id = ids.at(offset);
                const int existingIndex = guard->indexOfPost(id);
                if ((existingIndex >= 0 && existingIndex != target)
                    || (!guard->postIds.at(target).isEmpty()
                        && guard->postIds.at(target) != id)) {
                    qCDebug(lcThreadTimelineTrace).nospace()
                        << "THREAD_CACHE_TAIL_SKIP source="
                        << static_cast<const void*>(guard.data())
                        << " reason=identity-collision target=" << target
                        << " id=" << shortId(id);
                    return;
                }
            }

            for (const QString& id : ids) {
                if (guard->indexOfPost(id) < 0) {
                    guard->provisionalPostIds.insert(id);
                }
            }
            guard->rememberCursorCreateAt(ids);
            const ExactWindowMutation mutation = guard->assignExactWindow(first, ids);
            guard->publishExactWindow(mutation);
            qCDebug(lcThreadTimelineTrace).nospace()
                << "THREAD_CACHE_TAIL_HYDRATE source="
                << static_cast<const void*>(guard.data())
                << " target=[" << first << ',' << (first + usableCount - 1) << ']'
                << " ids=" << idsSummary(ids);
            guard->validateCachedTail();
        });
}

void ThreadPostSource::validateCachedTail()
{
    BackendPost* root = rootPost();
    if (!root) {
        return;
    }

    QPointer<ThreadPostSource> guard(this);
    if (static_cast<int>(postIds.size()) - 1 <= ServerBlockSize) {
        PostTimelineService::instance(backend).loadThreadPage(
            channel, rootId, ServerBlockSize, QString(), 0,
            [guard](const PostTimelineService::Page& result) {
                if (!guard || !result.success || result.postIds.isEmpty()) {
                    return;
                }
                guard->rememberCursorCreateAt(result.createAtById);
                guard->placeInitial(result.postIds);
            });
        return;
    }

    PostTimelineService::instance(backend).loadThreadTail(
        channel, rootId, ServerBlockSize,
        [guard](const PostTimelineService::Page& result) {
            if (!guard || !result.success || result.postIds.isEmpty()) {
                return;
            }
            guard->rememberCursorCreateAt(result.createAtById);
            guard->placeTail(result.postIds);
        });
}

bool ThreadPostSource::isAuthoritativeIndex(int index) const
{
    if (index < 0 || index >= static_cast<int>(postIds.size())) {
        return false;
    }
    const QString& id = postIds.at(index);
    return !id.isEmpty() && !provisionalPostIds.contains(id);
}

bool ThreadPostSource::isCursorReadyIndex(int index) const
{
    // Cursor authority is source metadata, not widget/body residency. A fetched
    // off-screen page may be evicted immediately under the post-body memory
    // policy, but its confirmed identity + create_at remain a valid Mattermost
    // compound cursor and must keep a random seek converging.
    return isAuthoritativeIndex(index) && cursorCreateAt(index) != 0;
}

uint64_t ThreadPostSource::cursorCreateAt(int index) const
{
    if (index < 0 || index >= static_cast<int>(postIds.size())) {
        return 0;
    }
    const QString& id = postIds.at(index);
    if (id.isEmpty()) {
        return 0;
    }
    const auto known = cursorCreateAtById.constFind(id);
    if (known != cursorCreateAtById.cend() && *known != 0) {
        return *known;
    }
    BackendPost* post = channel.postIdToPost.value(id, nullptr);
    return post ? post->create_at : 0;
}

void ThreadPostSource::rememberCursorCreateAt(
    const QHash<QString, uint64_t>& createAtById)
{
    for (auto it = createAtById.constBegin(); it != createAtById.constEnd(); ++it) {
        if (*it != 0) {
            cursorCreateAtById.insert(it.key(), *it);
        }
    }
}

void ThreadPostSource::rememberCursorCreateAt(const QStringList& ids)
{
    // Resident bodies are only a fallback for identities created locally or by
    // older cache paths. REST/cache Page metadata is preferred because it is
    // captured before the residency policy can evict an off-screen body.
    for (const QString& id : ids) {
        BackendPost* post = channel.postIdToPost.value(id, nullptr);
        if (post && post->create_at != 0) {
            cursorCreateAtById.insert(id, post->create_at);
        }
    }
}

void ThreadPostSource::pruneProvisionalPostIds()
{
    for (auto it = provisionalPostIds.begin(); it != provisionalPostIds.end();) {
        if (indexOfPost(*it) < 0) {
            it = provisionalPostIds.erase(it);
        } else {
            ++it;
        }
    }

    if (navigationPlacement.isActive()) {
        const int index = indexOfPost(navigationPlacement.postId());
        if (index >= 0) {
            navigationPlacement.updateIndex(index);
        }
    }
}

bool ThreadPostSource::placeExactWindow(int first, const QStringList& ids)
{
    if (ids.isEmpty()) {
        return false;
    }

    rememberCursorCreateAt(ids);
    const auto previousIslands = islands;
    for (const auto& previous : previousIslands) {
        const auto* current = islandForPost(previous.targetId);
        if (!current) continue;
        const Island island = *current;
        int origin = -1;
        for (int offset = 0; offset < ids.size(); ++offset) {
            const int islandOffset = static_cast<int>(island.ids.indexOf(ids.at(offset)));
            if (islandOffset < 0) continue;
            const int candidate = first + offset - islandOffset;
            if (origin >= 0 && origin != candidate) return false;
            origin = candidate;
        }
        if (origin >= 0) {
            if (!placeIsland(island, origin)) return false;
        } else if (first <= island.last() + 1 && first + ids.size() >= island.first) {
            // Numeric contact is not adjacency proof: move the whole provisional
            // island with guard slots before publishing this unrelated exact page.
            if (!placeIsland(island, -1, first, first + ids.size() - 1)) return false;
        }
    }

    // Exact cursor ranks take precedence over a navigation estimate. Keep the
    // semantic navigation target tracked even if this page replaces its slot;
    // a later page containing that identity will confirm its actual rank.
    const ThreadNavigationPlacement::Confirmation confirmation =
        navigationPlacement.confirmExactWindow(first, ids);
    for (const QString& id : ids) {
        provisionalPostIds.remove(id);
    }

    const ExactWindowMutation mutation = assignExactWindow(first, ids);
    publishExactWindow(mutation);

    // If the server confirmed the estimated identity at exactly the same slot,
    // assignExactWindow() has no mapping delta and therefore emits nothing. The
    // row nevertheless changed from navigation metadata to a materializable
    // source item, so publish that availability transition explicitly.
    if (confirmation.isValid() && confirmation.wasEstimated
        && !mutation.mappingChanged
        && confirmation.authoritativeIndex >= 0
        && confirmation.authoritativeIndex < static_cast<int>(postIds.size())
        && isAvailable(confirmation.authoritativeIndex)) {
        emit rangeAvailable(confirmation.authoritativeIndex,
                            confirmation.authoritativeIndex);
    }

    pruneProvisionalPostIds();
    return true;
}

void ThreadPostSource::placeInitial(const QStringList& ids)
{
    if (postIds.isEmpty() || ids.isEmpty()) {
        return;
    }

    const int first = ids.first() == rootId ? 0 : 1;
    const int count = std::min(static_cast<int>(ids.size()),
                               static_cast<int>(postIds.size()) - first);
    if (count <= 0) {
        return;
    }
    const QStringList page = ids.mid(0, count);
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PLACE_INITIAL source=" << static_cast<const void*>(this)
        << " ids=" << idsSummary(page)
        << " target=[" << first << ',' << (first + count - 1) << ']'
        << " before=" << slotSummary(postIds);

    placeExactWindow(first, page);
    if (first > 0 && !postIds.isEmpty()) {
        postIds[0] = rootId;
        rebuildIndex();
        emit rangeAvailable(0, 0);
    }

    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PLACE_INITIAL_DONE source=" << static_cast<const void*>(this)
        << ' ' << slotSummary(postIds);
}

void ThreadPostSource::placeTail(const QStringList& ids)
{
    if (postIds.size() <= 1 || ids.isEmpty()) {
        return;
    }

    const int count = std::min(static_cast<int>(ids.size()),
                               static_cast<int>(postIds.size()) - 1);
    if (count <= 0) {
        return;
    }
    const int first = static_cast<int>(postIds.size()) - count;
    const QStringList page = ids.mid(ids.size() - count);
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PLACE_TAIL source=" << static_cast<const void*>(this)
        << " ids=" << idsSummary(page)
        << " target=[" << first << ',' << (first + count - 1) << ']'
        << " before=" << slotSummary(postIds);

    placeExactWindow(first, page);

    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PLACE_TAIL_DONE source=" << static_cast<const void*>(this)
        << ' ' << slotSummary(postIds);
}

int ThreadPostSource::estimatedIndexForPost(const BackendPost& post) const
{
    const int count = static_cast<int>(postIds.size());
    if (post.id == rootId || count <= 1) {
        return 0;
    }
    if (count == 2) {
        return 1;
    }

    BackendPost* root = rootPost();
    if (!root) {
        return std::max(1, count / 2);
    }
    const uint64_t oldest = root->create_at;
    const uint64_t newest = std::max(root->last_reply_at, oldest);
    if (newest <= oldest || post.create_at <= oldest) {
        return 1;
    }
    if (post.create_at >= newest) {
        return count - 1;
    }

    const long double fraction = static_cast<long double>(post.create_at - oldest)
        / static_cast<long double>(newest - oldest);
    return 1 + static_cast<int>(std::llround(fraction * (count - 2)));
}

void ThreadPostSource::appendLiveReply(BackendPost& post)
{
    if (post.root_id != rootId) {
        return;
    }

    const int existing = indexOfPost(post.id);
    if (existing >= 0) {
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_LIVE_EXISTING source=" << static_cast<const void*>(this)
            << " post=" << shortId(post.id)
            << " index=" << existing;
        return;
    }

    const int oldCount = static_cast<int>(postIds.size());
    int count = currentLogicalCount();

    // BackendChannel::addPost() advances the root thread summary before the
    // posted event is forwarded as onNewPost(reply). onThreadSummaryChanged()
    // therefore normally grows postIds first, leaving one empty newest slot for
    // this exact live reply. Do not count the same reply twice.
    const bool metadataReservedTail = count == oldCount && count > 1
        && postIds.at(count - 1).isEmpty();
    if (count > oldCount) {
        resizeLogicalTail(count);
    } else if (!metadataReservedTail) {
        // Defensive fallback for a producer that delivers the live reply before
        // root metadata has advanced. In that ordering the event itself is the
        // only evidence that the logical thread grew.
        count = oldCount + 1;
        resizeLogicalTail(count);
    }
    const int index = count - 1;
    cursorCreateAtById.insert(post.id, post.create_at);
    publishExactWindow(assignExactWindow(index, QStringList { post.id }));
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_LIVE_APPEND source=" << static_cast<const void*>(this)
        << " post=" << shortId(post.id)
        << " index=" << index
        << ' ' << slotSummary(postIds);
}

} // namespace Mattermost
