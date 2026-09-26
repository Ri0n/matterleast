#include "ChannelTree.h"

#include <QMouseEvent>
#include <QPointer>
#include <QTimer>

#include "ChannelItem.h"
#include "backend/Backend.h"
#include "chat-area/ChatArea.h"

namespace Mattermost {

void ChannelTree::refreshCurrentChannelReadState(QTreeWidgetItem* item)
{
    if (!item || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {
        return;
    }

    const QString channelId = item->data(0, ItemIdRole).toString();
    ChatArea* page = getCurrentPage();
    if (!page || page->getChannel().id != channelId) {
        return;
    }

    // Selection/presentation is only a trigger to inspect the concrete viewport.
    // ChatLogWidget remains the sole owner of the lower-edge read rule and may
    // legitimately decide that nothing new has been read.
    page->refreshReadState();
}

void ChannelTree::currentChanged(const QModelIndex& current, const QModelIndex& previous)
{
    // Direct-message recency sorting moves existing QTreeWidgetItems with
    // takeChild()/insertChild(). Taking the current row temporarily detaches it
    // from the tree and Qt selects a neighbouring row. Treating that transient
    // model mutation as navigation activates an unrelated ChatArea even though
    // the user never selected it. Keep the visible conversation authoritative
    // until its row has been reinserted, then restore the tree current item.
    ChatArea* activePage = getCurrentPage();
    if (!renderingSidebar && activePage && activePage->treeItem
        && activePage->treeItem->treeWidget() != this) {
        QPointer<ChatArea> activeGuard(activePage);
        QTimer::singleShot(0, this, [this, activeGuard] {
            if (!activeGuard || !activeGuard->treeItem
                || activeGuard->treeItem->treeWidget() != this) {
                return;
            }
            setCurrentItem(activeGuard->treeItem);
        });
        return;
    }

    QTreeWidget::currentChanged(current, previous);

    // Sidebar rebuilds are programmatic and must never affect read progress.
    if (renderingSidebar || !current.isValid()) {
        return;
    }

    QTreeWidgetItem* item = itemFromIndex(current);
    if (!item || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {
        return;
    }

    const QString channelId = item->data(0, ItemIdRole).toString();
    if (channelId.isEmpty()) {
        return;
    }

    // A normal sidebar selection is navigation to the conversation itself, not
    // a semantic "first unread" command. The sparse channel controller therefore
    // opens its newest page at the bottom. Attention/permalink navigation owns
    // explicit unread/target positioning separately and installs a navigation
    // lock before its context is materialized.

    // currentItemChanged activates/materializes the ChatArea. Defer the viewport
    // re-check until that synchronous navigation path has completed, and resolve
    // the item again so category/sidebar rebuilds cannot leave a stale pointer.
    QTimer::singleShot(0, this, [this, channelId] {
        QTreeWidgetItem* currentItemPtr = currentItem();
        if (!currentItemPtr
            || currentItemPtr->data(0, ItemKindRole).toInt() != ChannelItemKind
            || currentItemPtr->data(0, ItemIdRole).toString() != channelId) {
            return;
        }
        refreshCurrentChannelReadState(currentItemPtr);
    });
}

void ChannelTree::mousePressEvent(QMouseEvent* event)
{
    QTreeWidgetItem* previousItem = currentItem();
    QTreeWidget::mousePressEvent(event);

    // currentChanged() handles normal navigation. A click on the already-current
    // row has no current-index transition, but can still repair a stale stacked-
    // page mismatch. Re-present it and then re-evaluate the resulting viewport;
    // neither action acknowledges read state by itself.
    if (!renderingSidebar && previousItem && previousItem == currentItem()) {
        QTreeWidgetItem* clickedItem = itemAt(event->pos());
        if (clickedItem == previousItem
            && clickedItem->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            activateChannelItem(clickedItem);
            refreshCurrentChannelReadState(clickedItem);
        }
    }
}


void ChannelTree::mouseMoveEvent(QMouseEvent* event)
{
    const QModelIndex index = indexAt(event->pos());
    const bool actionable = index.isValid()
        && index.data(SidebarItem::CategoryActionRole).toBool();
    const QRect rowRect = actionable ? visualRect(index) : QRect();
    const QRect actionRect = actionable
        ? QRect(rowRect.right() - 30 + 1, rowRect.top(), 30, rowRect.height())
        : QRect();
    const bool hovered = actionable && actionRect.contains(event->pos());

    if (index != categoryActionHoverIndex || hovered != categoryActionHovered) {
        const QPersistentModelIndex previous = categoryActionHoverIndex;
        if (previous.isValid()) {
            model()->setData(previous, false, SidebarItem::CategoryActionHoveredRole);
        }
        categoryActionHoverIndex = index;
        categoryActionHovered = hovered;
        if (index.isValid()) {
            model()->setData(index, hovered, SidebarItem::CategoryActionHoveredRole);
        }
    }

    QTreeWidget::mouseMoveEvent(event);
}


void ChannelTree::leaveEvent(QEvent* event)
{
    if (categoryActionHoverIndex.isValid()) {
        model()->setData(categoryActionHoverIndex, false,
                         SidebarItem::CategoryActionHoveredRole);
    }
    categoryActionHoverIndex = QPersistentModelIndex();
    categoryActionHovered = false;
    QTreeWidget::leaveEvent(event);
}

} // namespace Mattermost
