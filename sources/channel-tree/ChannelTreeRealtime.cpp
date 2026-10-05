/**
 * @file ChannelTreeRealtime.cpp
 * @brief Reconcile realtime DM/GM channels with the server-backed sidebar.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#include "ChannelTree.h"

#include "backend/Backend.h"
#include "backend/DirectConversationSidebarPolicy.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/team-item/TeamItem.h"

namespace Mattermost {

void ChannelTree::admitStoredConversation(BackendChannel& channel)
{
    if (!backendForSidebar
        || (channel.type != BackendChannel::directChannel
            && channel.type != BackendChannel::groupChannel)) {
        return;
    }

    auto& sidebar = SidebarService::instance(*backendForSidebar);

    // Direct/group conversations are global Mattermost conversations but the
    // sidebar exposes them through each team's server-backed Direct Messages
    // category. A direct_added or posted event may refer to a conversation that
    // has fallen outside the server's limited category snapshot. Promote it in
    // the local category and let the canonical reconciler materialize/reorder
    // the row without waiting for another HTTP round-trip.
    for (auto teamIt = teamToItemMap.begin(); teamIt != teamToItemMap.end(); ++teamIt) {
        TeamItem* teamItem = teamIt.value();
        SidebarTeamState* state = sidebar.teamState(teamIt.key());
        SidebarCategory* category = state
            ? state->categoryByType(QStringLiteral("direct_messages")) : nullptr;
        if (!teamItem || !category) {
            continue;
        }

        promoteSidebarConversation(category->channelIds, channel.id);

        if (sidebarDragActive) {
            pendingSidebarReconcileTeams.insert(teamIt.key());
            continue;
        }

        reconcileTeamSidebar(*backendForSidebar, *teamItem, *state);
    }
}

} // namespace Mattermost
