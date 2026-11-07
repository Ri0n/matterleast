#pragma once

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
                                   bool unread, bool mentioned, bool muted,
                                   const UnreadViewFilters& filters)
{
    // Muted is an independent unread source, including muted DMs/GMs.
    // Unmuted DMs/GMs and personal mentions belong only to Following.
    if (!tracked || !unread) return false;
    if (muted) return filters.muted;
    return filters.channels && isPublicOrPrivate && !mentioned;
}

} // namespace Mattermost
