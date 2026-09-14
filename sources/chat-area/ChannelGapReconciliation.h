#pragma once

#include <QString>
#include <QStringList>

namespace Mattermost::ChannelGapReconciliation {

struct ExactContextMatch {
    int first = -1;
    bool conflict = false;

    bool isExact() const { return first >= 0 && !conflict; }
};

inline ExactContextMatch matchAfter(int anchorIndex,
                                    const QStringList& pageIds,
                                    const QString& nextPostId,
                                    const QStringList& contextIds)
{
    ExactContextMatch result;
    for (int pageOffset = 0; pageOffset < pageIds.size(); ++pageOffset) {
        const int contextOffset = contextIds.indexOf(pageIds.at(pageOffset));
        if (contextOffset < 0) {
            continue;
        }
        const int candidate = anchorIndex + 1 + pageOffset - contextOffset;
        if (result.first < 0) {
            result.first = candidate;
        } else if (result.first != candidate) {
            result.conflict = true;
            return result;
        }
    }

    if (result.first < 0 && !contextIds.isEmpty()
        && nextPostId == contextIds.first()) {
        result.first = anchorIndex + 1 + pageIds.size();
    }
    return result;
}

inline ExactContextMatch matchBefore(int pageFirst,
                                     const QStringList& pageIds,
                                     const QString& prevPostId,
                                     const QStringList& contextIds)
{
    ExactContextMatch result;
    for (int pageOffset = 0; pageOffset < pageIds.size(); ++pageOffset) {
        const int contextOffset = contextIds.indexOf(pageIds.at(pageOffset));
        if (contextOffset < 0) {
            continue;
        }
        const int candidate = pageFirst + pageOffset - contextOffset;
        if (result.first < 0) {
            result.first = candidate;
        } else if (result.first != candidate) {
            result.conflict = true;
            return result;
        }
    }

    if (result.first < 0 && !contextIds.isEmpty()
        && prevPostId == contextIds.last()) {
        result.first = pageFirst - contextIds.size();
    }
    return result;
}

enum class GapDecision {
    None,
    ReserveOne,
    Reject,
};

inline GapDecision gapAfter(int pageLast,
                            int provisionalFirst,
                            const QString& nextPostId)
{
    if (!nextPostId.isEmpty()) {
        return GapDecision::ReserveOne;
    }
    return pageLast + 1 >= provisionalFirst
        ? GapDecision::Reject
        : GapDecision::None;
}

inline GapDecision gapBefore(int pageFirst,
                             int provisionalLast,
                             const QString& prevPostId)
{
    if (!prevPostId.isEmpty()) {
        return GapDecision::ReserveOne;
    }
    return pageFirst - 1 <= provisionalLast
        ? GapDecision::Reject
        : GapDecision::None;
}

struct GuardSlots {
    int left = 0;
    int right = 0;
};

inline GuardSlots provisionalGuards(bool reachedOldest, bool reachedNewest)
{
    return {reachedOldest ? 0 : 1, reachedNewest ? 0 : 1};
}

enum class AbsolutePageDecision {
    Place,
    Reconcile,
    Defer,
};

inline AbsolutePageDecision absolutePageDecision(int pageFirst,
                                                 int pageLast,
                                                 int provisionalFirst,
                                                 int provisionalLast,
                                                 bool touchesProvisionalIdentity)
{
    if (touchesProvisionalIdentity) {
        return AbsolutePageDecision::Reconcile;
    }
    const bool overlaps = pageFirst <= provisionalLast
        && pageLast >= provisionalFirst;
    const bool touches = pageLast + 1 == provisionalFirst
        || pageFirst == provisionalLast + 1;
    return overlaps || touches
        ? AbsolutePageDecision::Defer
        : AbsolutePageDecision::Place;
}

} // namespace Mattermost::ChannelGapReconciliation
