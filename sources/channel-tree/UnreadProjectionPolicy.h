#pragma once

#include <cstdint>

#include <QString>

namespace Mattermost {

// The Unread tab is a presentation union of the existing Following queue
// and unread public/private channels. These switches never change read state.
struct UnreadViewFilters {
    bool following = true;
    bool channels = true;
    bool muted = false;
};

inline bool includeFollowingInUnread(bool isThread, bool muted,
                                     const UnreadViewFilters& filters)
{
    // A followed thread is an explicit subscription even when its parent is
    // muted. Preserve the historical Following membership/visibility rules.
    return filters.following && (isThread || !muted || filters.muted);
}

inline bool includeChannelInUnread(bool isPublicOrPrivate, bool tracked,
                                   bool unread, bool muted,
                                   const UnreadViewFilters& filters)
{
    // Unread ownership comes from SidebarService, as in the Channels tree.
    // Mentions are separate Following thread/post rows, not necessarily a
    // Following conversation: filtering a mentioned channel here would hide
    // its other unread messages even while Channels still marks it unread.
    // The caller deduplicates c:<channelId> against Following conversations.
    // Muted remains an independent source, including muted DMs/GMs.
    if (!tracked || !unread) return false;
    if (muted) return filters.muted;
    return filters.channels && isPublicOrPrivate;
}

// Display priority is determined by the source, not by whether a channel is
// muted: unread Following entries always precede all other unread channels.
// Muted and ordinary unread channels share the same chronological group.
enum class UnreadSortGroup {
    UnreadFollowing,
    UnreadChannel,
    ReadFollowing,
};

inline UnreadSortGroup unreadSortGroup(bool fromFollowing, bool unread)
{
    if (!fromFollowing) return UnreadSortGroup::UnreadChannel;
    return unread ? UnreadSortGroup::UnreadFollowing
                  : UnreadSortGroup::ReadFollowing;
}

struct UnreadSortKey {
    UnreadSortGroup group = UnreadSortGroup::UnreadChannel;
    std::uint64_t activityAt = 0;
    QString identity;
};

inline bool unreadSortBefore(const UnreadSortKey& lhs, const UnreadSortKey& rhs)
{
    if (lhs.group != rhs.group) {
        return lhs.group < rhs.group;
    }
    if (lhs.activityAt != rhs.activityAt) {
        return lhs.activityAt > rhs.activityAt;
    }
    // A deterministic tie-breaker prevents rows with identical timestamps
    // from shuffling when the projection is refreshed.
    return lhs.identity < rhs.identity;
}

} // namespace Mattermost
