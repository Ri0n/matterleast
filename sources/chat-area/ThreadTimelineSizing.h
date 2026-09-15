#pragma once

#include <algorithm>
#include <cmath>
#include <vector>
#include <cstdint>
#include <limits>

namespace Mattermost {

/**
 * Convert Mattermost's live-reply metadata into the number of visible logical
 * thread rows. Deleted replies that the client still renders as tombstones are
 * deliberately additional rows: reply_count tracks live replies, not those
 * locally retained placeholders.
 */
inline int threadLogicalItemCount(std::int64_t liveReplies,
                                  int deletedReplyTombstones)
{
    const std::int64_t boundedLive = std::max<std::int64_t>(0, liveReplies);
    const std::int64_t boundedDeleted = std::max<std::int64_t>(0, deletedReplyTombstones);
    const std::int64_t maxReplies = std::numeric_limits<int>::max() - 1LL;
    const std::int64_t visibleReplies = std::min(
        maxReplies,
        boundedLive > maxReplies - std::min(boundedDeleted, maxReplies)
            ? maxReplies
            : boundedLive + boundedDeleted);
    return std::max(1, static_cast<int>(visibleReplies) + 1);
}

struct ThreadSeekAnchor {
    int index = 0;
    std::uint64_t createAt = 0;
};

struct ThreadSeekEstimate {
    std::uint64_t createAt = 0;
    int lowerIndex = 0;
    int upperIndex = 0;
};

/**
 * Estimate a server timestamp for a logical thread index.
 *
 * Thread activity is rarely uniform over time. Prefer the nearest known
 * logical/timestamp anchors and use root/latest-reply metadata only outside the
 * authoritative islands already discovered by the source.
 */
inline ThreadSeekEstimate threadSeekEstimate(
    int logicalIndex,
    int lastLogicalIndex,
    std::uint64_t rootCreateAt,
    std::uint64_t tailCreateAt,
    const std::vector<ThreadSeekAnchor>& anchors)
{
    if (lastLogicalIndex <= 0) {
        return ThreadSeekEstimate { rootCreateAt, 0, 0 };
    }

    logicalIndex = std::max(0, std::min(logicalIndex, lastLogicalIndex));
    ThreadSeekAnchor lower { 0, rootCreateAt };
    ThreadSeekAnchor upper { lastLogicalIndex, std::max(rootCreateAt, tailCreateAt) };

    for (const ThreadSeekAnchor& anchor : anchors) {
        if (anchor.index < 0 || anchor.index > lastLogicalIndex) {
            continue;
        }
        if (anchor.index <= logicalIndex && anchor.index >= lower.index) {
            lower = anchor;
        }
        if (anchor.index >= logicalIndex && anchor.index <= upper.index) {
            upper = anchor;
        }
    }

    ThreadSeekEstimate result;
    result.lowerIndex = lower.index;
    result.upperIndex = upper.index;

    if (lower.index == upper.index || logicalIndex <= lower.index
        || upper.createAt <= lower.createAt) {
        result.createAt = lower.createAt;
        return result;
    }
    if (logicalIndex >= upper.index) {
        result.createAt = upper.createAt;
        return result;
    }

    const long double fraction = static_cast<long double>(logicalIndex - lower.index)
        / static_cast<long double>(upper.index - lower.index);
    const long double estimate = static_cast<long double>(lower.createAt)
        + fraction * static_cast<long double>(upper.createAt - lower.createAt);
    result.createAt = static_cast<std::uint64_t>(std::llround(estimate));
    return result;
}

} // namespace Mattermost
