#pragma once

#include <QString>

namespace Mattermost {

// A navigation tab is an independent central destination; a pinned one must
// never be replaced as a side effect of opening a followed/attention thread.
inline bool shouldOpenThreadParentChannel(bool enabled, bool activeTabPinned,
                                          bool centralSurfaceIsMainStack,
                                          const QString& currentChannelId,
                                          const QString& threadChannelId)
{
    return enabled && !activeTabPinned && centralSurfaceIsMainStack
        && !threadChannelId.isEmpty() && currentChannelId != threadChannelId;
}

// The dock belongs to the current channel, DM or GM. Detached/thread tabs
// remain independent and are not governed by this presentation-only policy.
inline bool shouldCloseThreadDockForChannel(bool enabled,
                                           const QString& currentChannelId,
                                           const QString& dockedThreadChannelId)
{
    return enabled && !currentChannelId.isEmpty()
        && !dockedThreadChannelId.isEmpty()
        && currentChannelId != dockedThreadChannelId;
}

} // namespace Mattermost
