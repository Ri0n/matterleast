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

    // Direct/group conversations are global Mattermost conversations. A
    // direct_added or posted event may refer to one that has fallen outside the
    // visible Direct Messages limit. Preserve any explicit Favorites/custom
    // category placement; only conversations absent from every server category
    // are admitted into Direct Messages locally.
    for (auto teamIt = teamToItemMap.begin(); teamIt != teamToItemMap.end(); ++teamIt) {
        TeamItem* teamItem = teamIt.value();
        SidebarTeamState* state = sidebar.teamState(teamIt.key());
        if (!teamItem || !state) {
            continue;
        }

        SidebarCategory* directCategory = state->categoryByType(
            QStringLiteral("direct_messages"));
        bool presentInAnyCategory = false;
        bool presentInDirectCategory = false;
        for (auto categoryIt = state->categories.cbegin();
             categoryIt != state->categories.cend(); ++categoryIt) {
            if (!categoryIt->channelIds.contains(channel.id)) {
                continue;
            }
            presentInAnyCategory = true;
            if (categoryIt->type == QStringLiteral("direct_messages")) {
                presentInDirectCategory = true;
            }
        }

        if (directCategory && (!presentInAnyCategory || presentInDirectCategory)) {
            promoteSidebarConversation(directCategory->channelIds, channel.id);
        } else if (!presentInAnyCategory) {
            // No loaded category can represent the new conversation yet. The
            // next authoritative sidebar response will admit it.
            continue;
        }

        if (sidebarDragActive) {
            pendingSidebarReconcileTeams.insert(teamIt.key());
            continue;
        }

        reconcileTeamSidebar(*backendForSidebar, *teamItem, *state);
    }
}

} // namespace Mattermost
