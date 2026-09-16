/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ChannelTree.h"

#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelItem.h"

namespace Mattermost {

void ChannelTree::handleChannelUpdated()
{
    auto* channel = qobject_cast<BackendChannel*>(sender());
    if (!channel) {
        return;
    }

    const auto items = channelToItemMap.value(channel->id);
    for (QTreeWidgetItem* row : items) {
        if (!row || row->data(0, ItemKindRole).toInt() != ChannelItemKind) {
            continue;
        }
        static_cast<ChannelItem*>(row)->setLabel(channel->display_name);
    }
}

} // namespace Mattermost
