#pragma once

#include <algorithm>

namespace Mattermost {

struct ChannelMemberPageDecision
{
    int availableCount = 0;
    bool acceptPage = false;
};

/**
 * Validate an offset-paginated member page against the authoritative count.
 *
 * A short page in the middle of a large channel is not evidence of the end of
 * the member list: it can be a failed/empty transport response or a transient
 * paging inconsistency. In either case the caller must keep the count obtained
 * from /stats and leave the page retryable.
 */
inline ChannelMemberPageDecision channelMemberPageDecision(int memberCount,
                                                           int pageStart,
                                                           int pageSize,
                                                           int returnedCount)
{
    ChannelMemberPageDecision result;
    if (memberCount <= 0 || pageStart < 0 || pageSize <= 0
        || pageStart >= memberCount || returnedCount < 0) {
        return result;
    }

    const int expectedCount = std::min(pageSize, memberCount - pageStart);
    if (returnedCount < expectedCount) {
        return result;
    }

    result.availableCount = expectedCount;
    result.acceptPage = true;
    return result;
}

} // namespace Mattermost
