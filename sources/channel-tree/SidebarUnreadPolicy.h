#pragma once

namespace Mattermost::SidebarUnreadPolicy {

inline bool followingVisible(bool unreadOnly, bool channelsOnly)
{
    return !unreadOnly || channelsOnly;
}

inline bool unreadGateActive(bool unreadOnly,
                             bool textFilterActive,
                             bool ignoreWhileFiltering)
{
    return unreadOnly && !(textFilterActive && ignoreWhileFiltering);
}

inline bool anyFilterActive(bool unreadOnly,
                            bool textFilterActive,
                            bool ignoreWhileFiltering)
{
    return unreadGateActive(unreadOnly, textFilterActive, ignoreWhileFiltering)
        || textFilterActive;
}

// Personal/Saved/Drafts/Recent Mentions are local navigation shortcuts rather
// than unread-able channels. The unread gate never removes them; an explicit
// text filter may still hide shortcuts whose label does not match.
inline bool virtualDestinationVisible(bool textFilterActive, bool textMatches)
{
    return !textFilterActive || textMatches;
}

} // namespace Mattermost::SidebarUnreadPolicy
