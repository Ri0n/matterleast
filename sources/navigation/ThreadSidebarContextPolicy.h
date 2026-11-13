#pragma once

#include <QString>

#include "navigation/NavigationTabsModel.h"

namespace Mattermost {

// Contextual thread navigation is allowed to change tabs, but must never put
// an unrelated thread beside a pinned channel or an independent thread tab.
enum class ThreadParentTabRoute {
    KeepCurrent,
    ActivateExisting,
    ReuseUnpinned,
    CreateNew
};

struct ThreadParentTabDecision {
    ThreadParentTabRoute route = ThreadParentTabRoute::KeepCurrent;
    int index = -1;
};

// Explicit thread-only tabs remain independent when opened directly. Ordinary
// contextual navigation reuses an existing parent-channel tab first; otherwise
// it replaces an unpinned tab (prefer the active one) or creates a new tab when
// every existing tab is pinned.
inline ThreadParentTabDecision chooseThreadParentTab(
    bool enabled, const NavigationTabsModel& tabs, int activeIndex,
    const QString& channelId, const QString& rootId)
{
    if (!enabled || channelId.isEmpty() || rootId.isEmpty()) {
        return {};
    }
    if (const auto* active = tabs.at(activeIndex);
        active && active->channelId == channelId
        && (active->rootId.isEmpty() || active->rootId == rootId)) {
        return {};
    }
    const int existing = tabs.findDestination(channelId, QString());
    if (existing >= 0) {
        return {ThreadParentTabRoute::ActivateExisting, existing};
    }
    if (const auto* active = tabs.at(activeIndex);
        active && !active->pinned) {
        return {ThreadParentTabRoute::ReuseUnpinned, activeIndex};
    }
    for (int i = 0; i < tabs.count(); ++i) {
        if (const auto* entry = tabs.at(i); entry && !entry->pinned) {
            return {ThreadParentTabRoute::ReuseUnpinned, i};
        }
    }
    return {ThreadParentTabRoute::CreateNew, -1};
}

// The central channel must match a docked thread whenever contextual mode is
// enabled. Pinned tabs are protected by tab selection, not by allowing a dock
// from a foreign channel.
inline bool shouldOpenThreadParentChannel(bool enabled,
                                           bool centralSurfaceIsMainStack,
                                           const QString& currentChannelId,
                                           const QString& threadChannelId)
{
    return enabled && centralSurfaceIsMainStack
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
