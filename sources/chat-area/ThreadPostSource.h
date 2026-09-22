#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <QHash>
#include <QSet>
#include <QString>

#include "IndexedPostSource.h"
#include "ThreadNavigationPlacement.h"
#include "backend/PostResidencyLease.h"

namespace Mattermost {

class Backend;
class BackendChannel;

/** Thread root + replies mapped onto stable oldest->newest logical indices. */
class ThreadPostSource : public IndexedPostSource
{
    Q_OBJECT
public:
    explicit ThreadPostSource(Backend& backend,
                              BackendChannel& channel,
                              QString rootId,
                              QObject* parent = nullptr);

    bool isAvailable(int index) const override;
    int ensurePostIndex(const QString& postId) override;

    void requestRange(int first,
                      int last,
                      RequestReason reason,
                      quint64 generation) override;

    const QString& rootPostId() const { return rootId; }

    /** Whether this post already has a server-confirmed logical thread index. */
    bool isPostPositionAuthoritative(const QString& postId) const
    {
        const int index = indexOfPost(postId);
        return index >= 0 && !provisionalPostIds.contains(postId);
    }

    /** A bounded, renderable context is sufficient to present a permalink. */
    bool isPostReadyForNavigation(const QString& postId) const;

signals:
    /** A failed demand is explicit; a new user request can retry it. */
    void rangeRequestFailed(int first, int last, const QString& error);

private:
    static constexpr int ServerBlockSize = 10;

    BackendPost* rootPost() const;
    int currentLogicalCount() const;
    int nearestEmptyIndex(int preferred) const;
    void seedCachedPosts();
    void hydrateCachedTail();
    void validateCachedTail();
    bool isAuthoritativeIndex(int index) const;
    bool isCursorReadyIndex(int index) const;
    uint64_t cursorCreateAt(int index) const;
    void rememberCursorCreateAt(const QHash<QString, uint64_t>& createAtById);
    void rememberCursorCreateAt(const QStringList& ids);
    void pruneProvisionalPostIds();
    void retainMappedTombstone(const QString& postId);
    struct Demand;
    void continueDemand(const std::shared_ptr<Demand>& demand);
    void finishDemand(const std::shared_ptr<Demand>& demand, const QString& error = {});
    void failDemand(const std::shared_ptr<Demand>& demand, const QString& error);
    bool placeExactWindow(int first, const QStringList& ids);
    struct Island {
        QString targetId;
        QStringList ids;
        int first = -1;
        bool ready = false;
        bool reachedOldest = false;
        bool reachedNewest = false;
        int last() const { return first + static_cast<int>(ids.size()) - 1; }
    };
    struct IslandLoad;
    void continueIslandDemand(const std::shared_ptr<Demand>& demand);
    bool placeIsland(Island next, int exactFirst = -1,
                     int reservedFirst = -1, int reservedLast = -1);
    const Island* islandForPost(const QString& postId) const;
    QVector<Island> islands;
    void seedSeekIsland(const std::shared_ptr<Demand>& demand);
    std::shared_ptr<IslandLoad> islandLoad;
    quint64 islandEpoch = 0;
    void placeInitial(const QStringList& ids);
    void placeTail(const QStringList& ids);
    int estimatedIndexForPost(const BackendPost& post) const;
    void appendLiveReply(BackendPost& post);

    Backend& backend;
    QString rootId;
    PostResidencyLease rootResidencyLease;
    std::vector<PostResidencyLease> tombstoneResidencyLeases;
    QSet<QString> leasedTombstoneIds;
    QHash<QString, uint64_t> cursorCreateAtById;
    QSet<QString> provisionalPostIds;
    ThreadNavigationPlacement navigationPlacement;
    quint64 latestGeneration = 0;
    std::vector<std::weak_ptr<Demand>> activeDemands;
};

} // namespace Mattermost
