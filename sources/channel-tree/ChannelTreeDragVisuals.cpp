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

    const QPoint cursorInViewport = viewport()->mapFromGlobal(QCursor::pos());
    dragStartPointerY = cursorInViewport.y();
    draggedBlockHotSpotY = blockRect.isValid()
        ? qBound(0, cursorInViewport.y() - blockRect.top(), qMax(0, blockRect.height() - 1))
        : 0;

    const QRect previewRect = blockRect.intersected(viewport()->rect());
    QDrag drag(this);
    drag.setMimeData(mimeData);
    if (previewRect.isValid() && !previewRect.isEmpty()) {
        drag.setPixmap(viewport()->grab(previewRect));
        QPoint hotSpot = cursorInViewport - previewRect.topLeft();
        hotSpot.setX(qBound(0, hotSpot.x(), previewRect.width() - 1));
        hotSpot.setY(qBound(0, hotSpot.y(), previewRect.height() - 1));
        drag.setHotSpot(hotSpot);
    }

    ensureDragSourceVisuals(source);
    drag.exec(Qt::MoveAction, Qt::MoveAction);
    if (!dragSourceIndexes.isEmpty() || !dragGapIndexes.isEmpty()) {
        resetDragVisuals(false);
    }
}

void ChannelTree::dragLeaveEvent(QDragLeaveEvent* event)
{
    QTreeWidget::dragLeaveEvent(event);
    restoreSourceDropGap(true);
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

    QTreeWidgetItem* originalAnchor = nullptr;
    bool originalGapAfter = false;
    if (source->data(0, ItemKindRole).toInt() == CategoryItemKind) {
        prepareCategoryDragBoundaries(source);
        if (const CategoryDragBoundary* boundary =
                nearestCategoryDragBoundary(draggedBlockStartLogicalY)) {
            QTreeWidgetItem* category = boundary->targetCategory.isValid()
                ? itemFromIndex(boundary->targetCategory) : nullptr;
            if (category) {
                originalGapAfter = boundary->afterTarget;
                originalAnchor = categoryDropGapAnchor(
                    category, boundary->afterTarget, originalGapAfter);
            }
        }
    } else {
        originalAnchor = sourceDropGapAnchor(source, originalGapAfter);
    }

    // Exactly as in AnyKeep: the drag preview is now the only painted copy of
    // the source.  Its layout extent is still intact at this instant and will
    // be animated out together with the original insertion gap.
    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        if (index.isValid()) {
            model()->setData(index, true, SidebarItem::DragSourceHiddenRole);
        }
    }
    viewport()->update();

    dragGapIndexes.clear();
    currentDragGapIndex = QPersistentModelIndex();
    currentDragGapAfter = false;
    currentDragGapExtent = 0;
    sourceDragGapIndex = originalAnchor
        ? QPersistentModelIndex(indexFromItem(originalAnchor, 0))
        : QPersistentModelIndex();
    sourceDragGapAfter = originalGapAfter;

    if (sourceDragGapIndex.isValid()) {
        animateDropGap(sourceDragGapIndex, sourceDragGapAfter, draggedRowExtent);
    }
}

QTreeWidgetItem* ChannelTree::sourceDropGapAnchor(QTreeWidgetItem* source,
                                                   bool& gapAfter) const
{
    gapAfter = false;
    if (!source || !source->parent()) {
        return nullptr;
    }

    QTreeWidgetItem* parent = source->parent();
    const int row = parent->indexOfChild(source);
    if (row < 0) {
        return nullptr;
    }

    // The source-removed boundary at its old position is "before next" when a
    // next row exists, otherwise "after previous".  This is the same boundary
    // representation used by AnyKeep's LinearReorderLayout.
    for (int i = row + 1; i < parent->childCount(); ++i) {
        QTreeWidgetItem* sibling = parent->child(i);
        if (sibling && !sibling->isHidden()) {
            gapAfter = false;
            return sibling;
        }
    }
    for (int i = row - 1; i >= 0; --i) {
        QTreeWidgetItem* sibling = parent->child(i);
        if (sibling && !sibling->isHidden()) {
            gapAfter = true;
            return sibling;
        }
    }

    // Single channel in a category: the category header is the remaining
    // boundary and the gap belongs immediately after it.
    gapAfter = true;
    return parent;
}

void ChannelTree::prepareCategoryDragBoundaries(QTreeWidgetItem* source)
{
    categoryDragBoundaries.clear();
    draggedBlockStartLogicalY = 0;
    if (!source || !source->parent()) {
        return;
    }

    QTreeWidgetItem* team = source->parent();
    int logicalY = 0;
    QPersistentModelIndex lastCategory;

    auto blockExtent = [this](QTreeWidgetItem* category) {
        if (!category) {
            return 0;
        }
        int extent = 0;
        const QRect categoryRect = visualItemRect(category);
        if (categoryRect.isValid() && categoryRect.height() > 0) {
            extent += categoryRect.height();
        }
        if (category->isExpanded()) {
            for (int i = 0; i < category->childCount(); ++i) {
                QTreeWidgetItem* child = category->child(i);
                if (!child || child->isHidden()) {
                    continue;
                }
                const QRect childRect = visualItemRect(child);
                if (childRect.isValid() && childRect.height() > 0) {
                    extent += childRect.height();
                }
            }
        }
        return extent;
    };

    // "before every remaining item, after the final item", exactly matching
    // LinearReorderLayout.boundaries().  The source contributes no extent.
    for (int i = 0; i < team->childCount(); ++i) {
        QTreeWidgetItem* category = team->child(i);
        if (!category || category->isHidden()
            || category->data(0, ItemKindRole).toInt() != CategoryItemKind) {
            continue;
        }

        if (category == source) {
            draggedBlockStartLogicalY = logicalY;
            continue;
        }

        CategoryDragBoundary boundary;
        boundary.position = logicalY;
        boundary.targetCategory = QPersistentModelIndex(indexFromItem(category, 0));
        boundary.afterTarget = false;
        categoryDragBoundaries.push_back(boundary);

        logicalY += blockExtent(category);
        lastCategory = boundary.targetCategory;
    }

    if (lastCategory.isValid()) {
        CategoryDragBoundary trailing;
        trailing.position = logicalY;
        trailing.targetCategory = lastCategory;
        trailing.afterTarget = true;
        categoryDragBoundaries.push_back(trailing);
    }
}

const ChannelTree::CategoryDragBoundary*
ChannelTree::nearestCategoryDragBoundary(int probe) const
{
    const CategoryDragBoundary* best = nullptr;
    int bestDistance = 0;
    for (const CategoryDragBoundary& boundary : categoryDragBoundaries) {
        if (!boundary.targetCategory.isValid()) {
            continue;
        }
        const int distance = qAbs(probe - boundary.position);
        if (!best || distance < bestDistance) {
            best = &boundary;
            bestDistance = distance;
        }
    }
    return best;
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

void ChannelTree::animateDropGap(const QPersistentModelIndex& target,
                                     bool after, int extent)
{
    stopAnimation(dropGapAnimation);
    stopAnimation(sourceCollapseAnimation);

    QVector<QPersistentModelIndex> gapIndexes;
    for (const QPersistentModelIndex& index : dragGapIndexes) {
        if (index.isValid() && !gapIndexes.contains(index)) {
            gapIndexes.push_back(index);
        }
    }
    if (target.isValid() && !gapIndexes.contains(target)) {
        gapIndexes.push_back(target);
    }
    dragGapIndexes = gapIndexes;

    QVector<int> startBefore;
    QVector<int> startAfter;
    startBefore.reserve(gapIndexes.size());
    startAfter.reserve(gapIndexes.size());
    for (const QPersistentModelIndex& index : gapIndexes) {
        startBefore.push_back(gapValue(index, SidebarItem::DropGapBeforeRole));
        startAfter.push_back(gapValue(index, SidebarItem::DropGapAfterRole));
    }

    QVector<qreal> sourceStarts;
    sourceStarts.reserve(dragSourceIndexes.size());
    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        sourceStarts.push_back(collapseValue(index));
    }

    currentDragGapIndex = target;
    currentDragGapAfter = after;
    currentDragGapExtent = target.isValid() ? qMax(0, extent) : 0;

    if (gapIndexes.isEmpty() && dragSourceIndexes.isEmpty()) {
        return;
    }

    auto* animation = new QVariantAnimation(this);
    dropGapAnimation = animation;
    animation->setDuration(DragAnimationMs);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);

    const int wantedExtent = target.isValid() ? qMax(0, extent) : 0;
    const qreal wantedCollapse = target.isValid() ? 1.0 : 0.0;
    const auto sourceIndexes = dragSourceIndexes;

    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, animation, gapIndexes, startBefore, startAfter,
             target, after, wantedExtent, sourceIndexes, sourceStarts,
             wantedCollapse](const QVariant& value) {
        if (dropGapAnimation != animation) {
            return;
        }
        const qreal progress = value.toDouble();

        // Source collapse and gap displacement share one progress value. For a
        // group every visible row in the dragged subtree uses that same value,
        // so the removed block extent equals the opened gap extent per frame.
        for (qsizetype i = 0; i < sourceIndexes.size(); ++i) {
            if (!sourceIndexes[i].isValid()) {
                continue;
            }
            const qreal current = sourceStarts[i]
                + (wantedCollapse - sourceStarts[i]) * progress;
            model()->setData(sourceIndexes[i], current,
                             SidebarItem::DragCollapseRole);
        }

        for (qsizetype i = 0; i < gapIndexes.size(); ++i) {
            if (!gapIndexes[i].isValid()) {
                continue;
            }
            const bool isTarget = gapIndexes[i] == target;
            const int wantedBefore = isTarget && !after ? wantedExtent : 0;
            const int wantedAfter = isTarget && after ? wantedExtent : 0;
            model()->setData(gapIndexes[i],
                             qRound(startBefore[i]
                                    + (wantedBefore - startBefore[i]) * progress),
                             SidebarItem::DropGapBeforeRole);
            model()->setData(gapIndexes[i],
                             qRound(startAfter[i]
                                    + (wantedAfter - startAfter[i]) * progress),
                             SidebarItem::DropGapAfterRole);
        }

        doItemsLayout();
        viewport()->update();
    });

    connect(animation, &QVariantAnimation::finished, this,
            [this, animation, gapIndexes, target] {
        if (dropGapAnimation != animation) {
            return;
        }
        for (const QPersistentModelIndex& index : gapIndexes) {
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

void ChannelTree::restoreSourceDropGap(bool animate)
{
    if (dragSourceIndexes.isEmpty() || !sourceDragGapIndex.isValid()) {
        return;
    }

    if (currentDragGapIndex == sourceDragGapIndex
        && currentDragGapAfter == sourceDragGapAfter
        && currentDragGapExtent == draggedRowExtent) {
        return;
    }

    if (animate) {
        animateDropGap(sourceDragGapIndex, sourceDragGapAfter, draggedRowExtent);
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
    dragGapIndexes.clear();
    dragGapIndexes.push_back(sourceDragGapIndex);
    model()->setData(sourceDragGapIndex,
                     sourceDragGapAfter ? 0 : draggedRowExtent,
                     SidebarItem::DropGapBeforeRole);
    model()->setData(sourceDragGapIndex,
                     sourceDragGapAfter ? draggedRowExtent : 0,
                     SidebarItem::DropGapAfterRole);
    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        if (index.isValid()) {
            model()->setData(index, 1.0, SidebarItem::DragCollapseRole);
        }
    }
    currentDragGapIndex = sourceDragGapIndex;
    currentDragGapAfter = sourceDragGapAfter;
    currentDragGapExtent = draggedRowExtent;
    doItemsLayout();
    viewport()->update();
}

void ChannelTree::clearDropGap(bool animate)
{
    if (!dragSourceIndexes.isEmpty()) {
        restoreSourceDropGap(animate);
        return;
    }

    if (dragGapIndexes.isEmpty()) {
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
        restoreSourceDropGap(true);
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
            model()->setData(index, false, SidebarItem::DragSourceHiddenRole);
        }
    }
    dragGapIndexes.clear();
    currentDragGapIndex = QPersistentModelIndex();
    sourceDragGapIndex = QPersistentModelIndex();
    currentDragGapAfter = false;
    sourceDragGapAfter = false;
    currentDragGapExtent = 0;
    dragSourceIndexes.clear();
    draggedRowExtent = 0;
    draggedBlockHotSpotY = 0;
    dragStartPointerY = 0;
    draggedBlockStartLogicalY = 0;
    categoryDragBoundaries.clear();
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
