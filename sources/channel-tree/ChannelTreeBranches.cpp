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

    QStyleOption option;
    option.initFrom(this);
    option.state |= QStyle::State_Children;
    if (isExpanded(index)) {
        option.state |= QStyle::State_Open;
    }

    // QTreeView normally gives PE_IndicatorBranch State_Item/State_Sibling as
    // well, which makes native styles draw the vertical/horizontal connector
    // lines. Keep only the current item's disclosure indicator and restrict it
    // to the final indentation cell so nested depth does not shift the arrow.
    const int indicatorWidth = qMin(indentation(), rect.width());
    option.rect = rect;
    if (layoutDirection() == Qt::LeftToRight) {
        option.rect.setLeft(rect.right() - indicatorWidth + 1);
    } else {
        option.rect.setRight(rect.left() + indicatorWidth - 1);
    }

    style()->drawPrimitive(QStyle::PE_IndicatorBranch, &option, painter, this);
}

} // namespace Mattermost
