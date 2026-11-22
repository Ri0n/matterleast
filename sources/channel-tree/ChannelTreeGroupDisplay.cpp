#include "ChannelTree.h"

#include <QPointer>

#include "ChannelItem.h"
#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

void ChannelTree::ensureGroupChannelDisplayName(const QString& channelId)
{
    ensureGroupChannelDisplayName(_boundAccountId, channelId);
}

void ChannelTree::ensureGroupChannelDisplayName(const QString& accountId,
                                                const QString& channelId)
{
    const ScopedId key {accountId.isEmpty() ? _boundAccountId : accountId, channelId};
    Backend* owner = backendForAccount(key.first);
    if (!owner || channelId.isEmpty())
        return;
    BackendChannel* channel = owner->getStorage().getChannelById(channelId);
    if (!channel || channel->type != BackendChannel::groupChannel)
        return;

    QPointer<ChannelTree> treeGuard(this);
    QPointer<Backend> ownerGuard(owner);
    QPointer<BackendChannel> channelGuard(channel);
    UserProfileService::instance(*owner).ensureGroupChannelMembers(
        *channel, [treeGuard, ownerGuard, channelGuard, key] {
            if (!treeGuard || !ownerGuard || !channelGuard
                || treeGuard->backendForAccount(key.first) != ownerGuard.data())
                return;

            const auto rows = treeGuard->channelToItemMap.value(key);
            for (QTreeWidgetItem* row : rows) {
                if (!row || treeGuard->backendForRow(row) != ownerGuard.data()
                    || row->data(0, SidebarItem::KindRole).toInt() != SidebarItem::Channel)
                    continue;
                static_cast<ChannelItem*>(row)->setLabel(channelGuard->display_name);
            }

            if (treeGuard->viewport())
                treeGuard->viewport()->update();
        });
}

} // namespace Mattermost
