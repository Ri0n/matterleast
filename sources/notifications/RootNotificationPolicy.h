#pragma once

namespace Mattermost {

// Desktop notification eligibility for NEW ROOT posts. Muted channels,
// own posts, already-active conversations and thread replies are filtered
// separately by MainWindow and are intentionally unaffected by this setting.
enum class ChannelRootNotificationMode {
    MentionsOnly = 0,
    MentionsOrFavorites = 1,
    AllUnmuted = 2,
};

inline ChannelRootNotificationMode channelRootNotificationModeFromSetting(int value)
{
    switch (value) {
    case 0: return ChannelRootNotificationMode::MentionsOnly;
    case 2: return ChannelRootNotificationMode::AllUnmuted;
    default: return ChannelRootNotificationMode::MentionsOrFavorites;
    }
}

inline bool shouldNotifyRootPost(bool directConversation,
                                 bool mentioned,
                                 bool favorite,
                                 ChannelRootNotificationMode mode)
{
    // Direct messages and group messages do not participate in the setting.
    if (directConversation || mentioned) return true;

    switch (mode) {
    case ChannelRootNotificationMode::MentionsOnly:
        return false;
    case ChannelRootNotificationMode::MentionsOrFavorites:
        return favorite;
    case ChannelRootNotificationMode::AllUnmuted:
        return true;
    }
    return favorite;
}

} // namespace Mattermost
