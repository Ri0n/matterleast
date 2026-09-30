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

} // namespace Mattermost::SidebarUnreadPolicy
