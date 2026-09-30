#pragma once

#include <array>
#include <algorithm>

#include "SidebarItem.h"

namespace Mattermost::VirtualDestinationBlock {

inline constexpr std::array<int, 4> Destinations {
    SidebarItem::PersonalDestination,
    SidebarItem::SavedDestination,
    SidebarItem::DraftsDestination,
    SidebarItem::RecentMentionsDestination,
};

inline constexpr int size()
{
    return static_cast<int>(Destinations.size());
}

inline bool contains(int destination)
{
    return std::find(Destinations.cbegin(), Destinations.cend(), destination)
        != Destinations.cend();
}

} // namespace Mattermost::VirtualDestinationBlock
