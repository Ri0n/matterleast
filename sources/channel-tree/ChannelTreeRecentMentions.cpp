#include "ChannelTree.h"

#include <QIcon>
#include <QTreeWidgetItem>
#include <QVariant>

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
    Backend& backend, TeamItem& teamItem, QTreeWidgetItem& categoryItem)
{
    auto* item = new RecentMentionsItem(backend, nullptr);
    categoryItem.addChild(item);
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
    item->setIcon(QIcon(QStringLiteral(":/icons/unread")));

    // Existing virtual destinations are dispatched from activateVirtualDestination().
    // Recent Mentions has no backing ChatArea/channel, so route its selection to
    // the same virtualDestinationRequested contract without inventing a channel.
    connect(this, &QTreeWidget::currentItemChanged,
            this, &ChannelTree::handleRecentMentionsSelection,
            Qt::UniqueConnection);
    return item;
}

void ChannelTree::handleRecentMentionsSelection(QTreeWidgetItem* current,
                                                QTreeWidgetItem*)
{
    if (!current
        || current->data(0, ItemKindRole).toInt() != VirtualDestinationItemKind
        || current->data(0, ItemDestinationRole).toInt()
            != SidebarItem::RecentMentionsDestination) {
        return;
    }

    emit virtualDestinationRequested(
        SidebarItem::RecentMentionsDestination,
        current->data(0, ItemTeamIdRole).toString());
}

} // namespace Mattermost
