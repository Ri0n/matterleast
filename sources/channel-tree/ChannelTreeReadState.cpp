#include "ChannelTree.h"

#include <QMouseEvent>
#include <QTimer>

#include "ChannelItem.h"
#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "chat-area/ChatArea.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {

void ChannelTree::refreshCurrentChannelReadState(QTreeWidgetItem* item)
{
    if (!item || item->data(0, ItemKindRole).toInt() != ChannelItemKind) {
        return;
    }

    const QString channelId = item->data(0, ItemIdRole).toString();
    ChatArea* page = getCurrentPage();
    if (!page || page->getChannel().id != channelId
        || backendForRow(item) != &page->getBackend()) {
        return;
    }

    // Selection/presentation is only a trigger to inspect the concrete viewport.
    // ChatLogWidget remains the sole owner of the lower-edge read rule and may
    // legitimately decide that nothing new has been read.
    page->refreshReadState();
}

void ChannelTree::currentChanged(const QModelIndex& current, const QModelIndex& previous)
{
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
    const QString accountId = item->data(0, ItemAccountIdRole).toString();
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
    QTimer::singleShot(0, this, [this, accountId, channelId] {
        QTreeWidgetItem* currentItemPtr = currentItem();
        if (!currentItemPtr
            || currentItemPtr->data(0, ItemKindRole).toInt() != ChannelItemKind
            || currentItemPtr->data(0, ItemIdRole).toString() != channelId
            || currentItemPtr->data(0, ItemAccountIdRole).toString() != accountId) {
            return;
        }
        refreshCurrentChannelReadState(currentItemPtr);
    });
}

void ChannelTree::mousePressEvent(QMouseEvent* event)
{
    if (!event) {
        return;
    }
    // A secondary click requests the context menu for the pointed row.
    // QTreeWidget's default press handling would select/activate it first,
    // making "Open in new tab" operate on an already-open conversation.
    // Qt still delivers the independent context-menu event.
    if (event->button() == Qt::RightButton) {
        event->accept();
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint eventPos = event->position().toPoint();
#else
    const QPoint eventPos = event->pos();
#endif

    if (!renderingSidebar && event->button() == Qt::MiddleButton && backendForSidebar) {
        QTreeWidgetItem* clickedItem = itemAt(eventPos);
        if (clickedItem) {
            QString channelId;
            const int kind = clickedItem->data(0, ItemKindRole).toInt();
            if (kind == ChannelItemKind) {
                channelId = clickedItem->data(0, ItemIdRole).toString();
            } else if (kind == VirtualDestinationItemKind
                       && clickedItem->data(0, ItemDestinationRole).toInt()
                           == SidebarItem::PersonalDestination) {
                channelId = clickedItem->data(0, ItemChannelIdRole).toString();
                if (channelId.isEmpty()) {
                    if (BackendChannel* personal =
                            backendForSidebar->getStorage().getDirectChannelByUserId(
                                backendForSidebar->getLoginUser().id)) {
                        channelId = personal->id;
                    }
                }
            }

            if (!channelId.isEmpty()) {
                AppNavigationService::instance(*backendForSidebar)
                    .openChannelInTab(channelId);
                event->accept();
                return;
            }
        }
    }

    QTreeWidgetItem* previousItem = currentItem();
    QTreeWidget::mousePressEvent(event);

    // currentChanged() handles normal navigation. A click on the already-current
    // row has no current-index transition, but can still repair a stale stacked-
    // page mismatch. Re-present it and then re-evaluate the resulting viewport;
    // neither action acknowledges read state by itself.
    if (!renderingSidebar && previousItem && previousItem == currentItem()) {
        QTreeWidgetItem* clickedItem = itemAt(eventPos);
        if (clickedItem == previousItem
            && clickedItem->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            activateChannelItem(clickedItem);
            refreshCurrentChannelReadState(clickedItem);
        }
    }
}


void ChannelTree::mouseMoveEvent(QMouseEvent* event)
{
    if (sidebarDragActive) {
        updateInternalDragPointer(event->pos());
        event->accept();
        return;
    }

    const QModelIndex index = indexAt(event->pos());
    const bool hasAdd = index.isValid()
        && index.data(SidebarItem::CategoryActionRole).toBool();
    const bool hasMenu = index.isValid()
        && index.data(SidebarItem::CategoryMenuRole).toBool();
    const QRect rowRect = (hasAdd || hasMenu) ? visualRect(index) : QRect();
    const auto accessoryRect = [&rowRect](int slot) {
        return QRect(rowRect.right() - (slot + 1) * 30 + 1,
                     rowRect.top(), 30, rowRect.height());
    };
    const bool addHovered = hasAdd && accessoryRect(0).contains(event->pos());
    // The options icon is revealed on category-row hover, not only after
    // the pointer finds its otherwise invisible 30 px click target.
    const bool menuHovered = hasMenu && rowRect.contains(event->pos());

    if (index != categoryActionHoverIndex
        || addHovered != categoryActionHovered
        || menuHovered != categoryMenuHovered) {
        const QPersistentModelIndex previous = categoryActionHoverIndex;
        if (previous.isValid() && previous != index) {
            model()->setData(previous, false, SidebarItem::CategoryActionHoveredRole);
            model()->setData(previous, false, SidebarItem::CategoryMenuHoveredRole);
        }
        categoryActionHoverIndex = index;
        categoryActionHovered = addHovered;
        categoryMenuHovered = menuHovered;
        if (index.isValid()) {
            model()->setData(index, addHovered, SidebarItem::CategoryActionHoveredRole);
            model()->setData(index, menuHovered, SidebarItem::CategoryMenuHoveredRole);
        }
    }

    QTreeWidget::mouseMoveEvent(event);
}


void ChannelTree::mouseReleaseEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::RightButton) {
        event->accept();
        return;
    }
    if (sidebarDragActive && event->button() == Qt::LeftButton) {
        finishInternalDrag(event->pos(), true);
        event->accept();
        return;
    }
    QTreeWidget::mouseReleaseEvent(event);
}

void ChannelTree::leaveEvent(QEvent* event)
{
    if (categoryActionHoverIndex.isValid()) {
        model()->setData(categoryActionHoverIndex, false,
                         SidebarItem::CategoryActionHoveredRole);
        model()->setData(categoryActionHoverIndex, false,
                         SidebarItem::CategoryMenuHoveredRole);
    }
    categoryActionHoverIndex = QPersistentModelIndex();
    categoryActionHovered = false;
    categoryMenuHovered = false;
    QTreeWidget::leaveEvent(event);
}

} // namespace Mattermost
