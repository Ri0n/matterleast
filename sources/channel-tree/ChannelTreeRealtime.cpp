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
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

void ChannelTree::admitStoredConversation(BackendChannel& channel)
{
    if (auto* owner = backendForStorage(channel.storage)) {
        auto& sidebar = SidebarService::instance(*owner);
        observeSidebarSignals(*owner, sidebar);
        sidebar.admitStoredConversation(channel);
    }
}

} // namespace Mattermost
