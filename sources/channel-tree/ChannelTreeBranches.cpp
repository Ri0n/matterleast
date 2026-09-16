#include "ChannelTree.h"

#include <QAbstractItemModel>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>

namespace Mattermost {

void ChannelTree::drawBranches(QPainter* painter, const QRect& rect,
                               const QModelIndex& index) const
{
    if (!painter || !index.isValid() || !model()->hasChildren(index)) {
        return;
    }

    QRect contentRect = rect;
    contentRect.adjust(0,
                       qMax(0, index.data(SidebarItem::DropGapBeforeRole).toInt()),
                       0,
                       -qMax(0, index.data(SidebarItem::DropGapAfterRole).toInt()));
    if (contentRect.height() <= 0) {
        return;
    }

    QStyleOption option;
    option.initFrom(this);

    const int extent = qMin(indentation(), contentRect.height());
    option.rect = QRect(0, 0, extent, extent);
    option.rect.moveCenter(contentRect.center());
    if (layoutDirection() == Qt::LeftToRight) {
        option.rect.moveLeft(contentRect.right() - extent + 1);
    } else {
        option.rect.moveRight(contentRect.left() + extent - 1);
    }

    const qreal collapse = qBound<qreal>(
        0.0, index.data(SidebarItem::DragCollapseRole).toDouble(), 1.0);
    painter->save();
    painter->setOpacity(painter->opacity() * (1.0 - collapse));
    style()->drawPrimitive(isExpanded(index)
                               ? QStyle::PE_IndicatorArrowDown
                               : QStyle::PE_IndicatorArrowRight,
                           &option, painter, this);
    painter->restore();
}

} // namespace Mattermost
