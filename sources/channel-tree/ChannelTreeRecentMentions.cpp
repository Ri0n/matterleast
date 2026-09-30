#include "ChannelTree.h"

#include <QTreeWidgetItem>
#include <QVariant>

#include "RecentMentionsIcon.h"
#include "backend/Backend.h"
#include "chat-area/ChatArea.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/team-item/TeamItem.h"

namespace Mattermost {
namespace {

class RecentMentionsItem final : public ChannelItem
{
public:
    using ChannelItem::ChannelItem;

    void showContextMenu(const QPoint&) override {}
};

} // namespace

ChannelItem* ChannelTree::createRecentMentionsItem(
    Backend& backend, TeamItem& teamItem, QTreeWidgetItem& parentItem)
{
    auto* item = new RecentMentionsItem(backend, nullptr);
    parentItem.addChild(item);
    item->setData(0, ItemKindRole, VirtualDestinationItemKind);
    item->setData(0, ItemIdRole, QStringLiteral("virtual:recent-mentions"));
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setData(0, ItemDestinationRole, SidebarItem::RecentMentionsDestination);
    item->setData(0, Qt::UserRole,
                  QVariant::fromValue(static_cast<ChatArea*>(nullptr)));
    item->setFlags(item->flags()
                   & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled
                       | Qt::ItemIsEditable));
    item->setLabel(tr("Recent Mentions"));
    item->setIcon(recentMentionsIcon(palette()));
    return item;
}

} // namespace Mattermost
