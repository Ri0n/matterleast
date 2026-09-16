/**
 * @file ChannelTreeDragVisuals.cpp
 * @brief Animated structural displacement for ChannelTree drag and drop.
 */

#include "ChannelTree.h"

#include <QCursor>
#include <QDrag>
#include <QDragLeaveEvent>
#include <QEasingCurve>
#include <QMimeData>
#include <QVariantAnimation>

namespace Mattermost {
namespace {

constexpr int DragAnimationMs = 160;

void stopAnimation(QVariantAnimation*& animation)
{
    if (!animation) {
        return;
    }
    animation->stop();
    animation->deleteLater();
    animation = nullptr;
}

qreal collapseValue(const QPersistentModelIndex& index)
{
    return index.isValid()
        ? qBound<qreal>(0.0, index.data(SidebarItem::DragCollapseRole).toDouble(), 1.0)
        : 0.0;
}

int gapValue(const QPersistentModelIndex& index, int role)
{
    return index.isValid() ? qMax(0, index.data(role).toInt()) : 0;
}

} // namespace

void ChannelTree::startDrag(Qt::DropActions supportedActions)
{
    if (!(supportedActions & Qt::MoveAction)) {
        return;
    }

    const auto selected = selectedItems();
    QTreeWidgetItem* source = selected.size() == 1 ? selected.front() : currentItem();
    if (!source) {
        return;
    }
    const int kind = source->data(0, ItemKindRole).toInt();
    if (kind != ChannelItemKind && kind != CategoryItemKind) {
        return;
    }

    const QModelIndex sourceIndex = indexFromItem(source, 0);
    if (!sourceIndex.isValid()) {
        return;
    }

    QModelIndexList indexes;
    indexes.push_back(sourceIndex);
    QMimeData* mimeData = model()->mimeData(indexes);
    if (!mimeData) {
        return;
    }

    // Capture the drag image before collapsing the source geometry. For an
    // expanded category the logical dragged block is the category plus all of
    // its children; the preview shows the visible portion of that subtree.
    QRect blockRect = visualItemRect(source);
    if (kind == CategoryItemKind && source->isExpanded()) {
        for (int i = 0; i < source->childCount(); ++i) {
            QTreeWidgetItem* child = source->child(i);
            if (!child || child->isHidden()) {
                continue;
            }
            const QRect childRect = visualItemRect(child);
            if (childRect.isValid() && childRect.height() > 0) {
                blockRect = blockRect.united(childRect);
            }
        }
    }

    const QRect previewRect = blockRect.intersected(viewport()->rect());
    QDrag drag(this);
    drag.setMimeData(mimeData);
    if (previewRect.isValid() && !previewRect.isEmpty()) {
        drag.setPixmap(viewport()->grab(previewRect));
        QPoint hotSpot = viewport()->mapFromGlobal(QCursor::pos()) - previewRect.topLeft();
        hotSpot.setX(qBound(0, hotSpot.x(), previewRect.width() - 1));
        hotSpot.setY(qBound(0, hotSpot.y(), previewRect.height() - 1));
        drag.setHotSpot(hotSpot);
    }

    ensureDragSourceVisuals(source);
    const Qt::DropAction result = drag.exec(Qt::MoveAction, Qt::MoveAction);
    if (!dragSourceIndexes.isEmpty() || !dragGapIndexes.isEmpty()) {
        resetDragVisuals(result == Qt::IgnoreAction);
    }
}

void ChannelTree::dragLeaveEvent(QDragLeaveEvent* event)
{
    QTreeWidget::dragLeaveEvent(event);
    clearDropGap(true);
}

void ChannelTree::ensureDragSourceVisuals(QTreeWidgetItem* source)
{
    if (!source) {
        return;
    }
    const QPersistentModelIndex sourceIndex(indexFromItem(source, 0));
    if (!dragSourceIndexes.isEmpty() && dragSourceIndexes.front() == sourceIndex) {
        return;
    }

    resetDragVisuals(false);
    draggedRowExtent = 0;

    auto append = [this](QTreeWidgetItem* row) {
        if (!row || row->isHidden()) {
            return;
        }
        const QModelIndex index = indexFromItem(row, 0);
        const QRect rect = visualItemRect(row);
        if (!index.isValid() || !rect.isValid() || rect.height() <= 0) {
            return;
        }
        dragSourceIndexes.push_back(QPersistentModelIndex(index));
        draggedRowExtent += rect.height();
    };

    append(source);
    if (source->data(0, ItemKindRole).toInt() == CategoryItemKind && source->isExpanded()) {
        for (int i = 0; i < source->childCount(); ++i) {
            append(source->child(i));
        }
    }

    if (draggedRowExtent <= 0) {
        draggedRowExtent = visualItemRect(source).height();
    }
    animateSourceCollapse(1.0);
}

void ChannelTree::animateSourceCollapse(qreal target)
{
    stopAnimation(sourceCollapseAnimation);
    if (dragSourceIndexes.isEmpty()) {
        return;
    }

    QVector<qreal> starts;
    starts.reserve(dragSourceIndexes.size());
    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        starts.push_back(collapseValue(index));
    }

    auto* animation = new QVariantAnimation(this);
    sourceCollapseAnimation = animation;
    animation->setDuration(DragAnimationMs);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);

    const auto indexes = dragSourceIndexes;
    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, animation, indexes, starts, target](const QVariant& value) {
        if (sourceCollapseAnimation != animation) {
            return;
        }
        const qreal progress = value.toReal();
        for (qsizetype i = 0; i < indexes.size(); ++i) {
            if (!indexes[i].isValid()) {
                continue;
            }
            const qreal current = starts[i] + (target - starts[i]) * progress;
            model()->setData(indexes[i], current, SidebarItem::DragCollapseRole);
        }
        doItemsLayout();
        viewport()->update();
    });
    connect(animation, &QVariantAnimation::finished, this,
            [this, animation, target] {
        if (sourceCollapseAnimation != animation) {
            return;
        }
        sourceCollapseAnimation = nullptr;
        animation->deleteLater();
        if (target <= 0.0) {
            dragSourceIndexes.clear();
            draggedRowExtent = 0;
        }
    });
    animation->start();
}

void ChannelTree::animateDropGap(const QPersistentModelIndex& target, bool after, int extent)
{
    stopAnimation(dropGapAnimation);

    QVector<QPersistentModelIndex> indexes;
    for (const QPersistentModelIndex& index : dragGapIndexes) {
        if (index.isValid() && !indexes.contains(index)) {
            indexes.push_back(index);
        }
    }
    if (target.isValid() && !indexes.contains(target)) {
        indexes.push_back(target);
    }
    dragGapIndexes = indexes;
    currentDragGapIndex = target;
    currentDragGapAfter = after;
    currentDragGapExtent = target.isValid() ? qMax(0, extent) : 0;

    if (indexes.isEmpty()) {
        return;
    }

    QVector<int> startBefore;
    QVector<int> startAfter;
    startBefore.reserve(indexes.size());
    startAfter.reserve(indexes.size());
    for (const QPersistentModelIndex& index : indexes) {
        startBefore.push_back(gapValue(index, SidebarItem::DropGapBeforeRole));
        startAfter.push_back(gapValue(index, SidebarItem::DropGapAfterRole));
    }

    auto* animation = new QVariantAnimation(this);
    dropGapAnimation = animation;
    animation->setDuration(DragAnimationMs);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);

    const int wantedExtent = qMax(0, extent);
    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, animation, indexes, startBefore, startAfter,
             target, after, wantedExtent](const QVariant& value) {
        if (dropGapAnimation != animation) {
            return;
        }
        const qreal progress = value.toDouble();
        for (qsizetype i = 0; i < indexes.size(); ++i) {
            if (!indexes[i].isValid()) {
                continue;
            }
            const bool isTarget = indexes[i] == target;
            const int wantedBefore = isTarget && !after ? wantedExtent : 0;
            const int wantedAfter = isTarget && after ? wantedExtent : 0;
            model()->setData(indexes[i],
                             qRound(startBefore[i]
                                    + (wantedBefore - startBefore[i]) * progress),
                             SidebarItem::DropGapBeforeRole);
            model()->setData(indexes[i],
                             qRound(startAfter[i]
                                    + (wantedAfter - startAfter[i]) * progress),
                             SidebarItem::DropGapAfterRole);
        }
        doItemsLayout();
        viewport()->update();
    });
    connect(animation, &QVariantAnimation::finished, this,
            [this, animation, indexes, target] {
        if (dropGapAnimation != animation) {
            return;
        }
        for (const QPersistentModelIndex& index : indexes) {
            if (index.isValid() && index != target) {
                model()->setData(index, 0, SidebarItem::DropGapBeforeRole);
                model()->setData(index, 0, SidebarItem::DropGapAfterRole);
            }
        }
        dropGapAnimation = nullptr;
        animation->deleteLater();
        dragGapIndexes.clear();
        if (target.isValid()) {
            dragGapIndexes.push_back(target);
        } else {
            currentDragGapIndex = QPersistentModelIndex();
            currentDragGapAfter = false;
            currentDragGapExtent = 0;
        }
    });
    animation->start();
}
void ChannelTree::updateDragVisuals(QTreeWidgetItem* source,
                                    QTreeWidgetItem* gapAnchor,
                                    bool gapAfter)
{
    ensureDragSourceVisuals(source);
    if (!gapAnchor || draggedRowExtent <= 0) {
        clearDropGap(true);
        return;
    }

    const QPersistentModelIndex target(indexFromItem(gapAnchor, 0));
    if (target == currentDragGapIndex
        && gapAfter == currentDragGapAfter
        && draggedRowExtent == currentDragGapExtent) {
        return;
    }
    animateDropGap(target, gapAfter, draggedRowExtent);
}

void ChannelTree::clearDropGap(bool animate)
{
    if (dragGapIndexes.isEmpty()) {
        return;
    }
    if (animate && !currentDragGapIndex.isValid()
        && currentDragGapExtent == 0 && dropGapAnimation) {
        return;
    }
    if (animate) {
        animateDropGap(QPersistentModelIndex(), false, 0);
        return;
    }

    stopAnimation(dropGapAnimation);
    for (const QPersistentModelIndex& index : dragGapIndexes) {
        if (index.isValid()) {
            model()->setData(index, 0, SidebarItem::DropGapBeforeRole);
            model()->setData(index, 0, SidebarItem::DropGapAfterRole);
        }
    }
    dragGapIndexes.clear();
    currentDragGapIndex = QPersistentModelIndex();
    currentDragGapAfter = false;
    currentDragGapExtent = 0;
}

void ChannelTree::resetDragVisuals(bool animate)
{
    if (animate) {
        clearDropGap(true);
        animateSourceCollapse(0.0);
        return;
    }

    stopAnimation(dropGapAnimation);
    stopAnimation(sourceCollapseAnimation);
    for (const QPersistentModelIndex& index : dragGapIndexes) {
        if (index.isValid()) {
            model()->setData(index, 0, SidebarItem::DropGapBeforeRole);
            model()->setData(index, 0, SidebarItem::DropGapAfterRole);
        }
    }
    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        if (index.isValid()) {
            model()->setData(index, 0.0, SidebarItem::DragCollapseRole);
        }
    }
    dragGapIndexes.clear();
    currentDragGapIndex = QPersistentModelIndex();
    currentDragGapAfter = false;
    currentDragGapExtent = 0;
    dragSourceIndexes.clear();
    draggedRowExtent = 0;
    doItemsLayout();
    viewport()->update();
}

QTreeWidgetItem* ChannelTree::channelDropGapAnchor(QTreeWidgetItem* source,
                                                   QTreeWidgetItem* targetCategoryItem,
                                                   const QString& targetChannelId,
                                                   bool afterTarget,
                                                   bool& gapAfter) const
{
    if (!targetCategoryItem) {
        return nullptr;
    }

    if (!targetChannelId.isEmpty()) {
        for (int i = 0; i < targetCategoryItem->childCount(); ++i) {
            QTreeWidgetItem* row = targetCategoryItem->child(i);
            if (row && row->data(0, ItemKindRole).toInt() == ChannelItemKind
                && row->data(0, ItemIdRole).toString() == targetChannelId) {
                gapAfter = afterTarget;
                return row;
            }
        }
    }

    // Dropping on a category header means append inside that category. Put the
    // structural opening after its last visible child, not between the header
    // and its children.
    if (targetCategoryItem->isExpanded()) {
        for (int i = targetCategoryItem->childCount() - 1; i >= 0; --i) {
            QTreeWidgetItem* row = targetCategoryItem->child(i);
            if (row && row != source && !row->isHidden()) {
                gapAfter = true;
                return row;
            }
        }
    }
    gapAfter = true;
    return targetCategoryItem;
}

QTreeWidgetItem* ChannelTree::categoryDropGapAnchor(QTreeWidgetItem* targetCategoryItem,
                                                    bool afterTarget,
                                                    bool& gapAfter) const
{
    if (!targetCategoryItem) {
        return nullptr;
    }
    if (afterTarget && targetCategoryItem->isExpanded()) {
        for (int i = targetCategoryItem->childCount() - 1; i >= 0; --i) {
            QTreeWidgetItem* row = targetCategoryItem->child(i);
            if (row && !row->isHidden()) {
                gapAfter = true;
                return row;
            }
        }
    }
    gapAfter = afterTarget;
    return targetCategoryItem;
}

} // namespace Mattermost
