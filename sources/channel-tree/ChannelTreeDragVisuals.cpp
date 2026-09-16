/**
 * @file ChannelTreeDragVisuals.cpp
 * @brief Animated structural displacement for ChannelTree drag and drop.
 */

#include "ChannelTree.h"

#include <QDragLeaveEvent>
#include <QEasingCurve>
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
        ? qBound<qreal>(0.0, index.data(SidebarItem::DragCollapseRole).toReal(), 1.0)
        : 0.0;
}

int gapValue(const QPersistentModelIndex& index, int role)
{
    return index.isValid() ? qMax(0, index.data(role).toInt()) : 0;
}

} // namespace

void ChannelTree::startDrag(Qt::DropActions supportedActions)
{
    QTreeWidget::startDrag(supportedActions);
    if (!dragSourceIndexes.isEmpty() || dragGapIndex.isValid()) {
        resetDragVisuals(true);
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
        for (int i = 0; i < indexes.size(); ++i) {
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

    const QPersistentModelIndex old = dragGapIndex;
    const int oldBefore = gapValue(old, SidebarItem::DropGapBeforeRole);
    const int oldAfter = gapValue(old, SidebarItem::DropGapAfterRole);
    const int newBefore = gapValue(target, SidebarItem::DropGapBeforeRole);
    const int newAfter = gapValue(target, SidebarItem::DropGapAfterRole);
    const int wantedBefore = target.isValid() && !after ? qMax(0, extent) : 0;
    const int wantedAfter = target.isValid() && after ? qMax(0, extent) : 0;

    dragGapIndex = target;
    auto* animation = new QVariantAnimation(this);
    dropGapAnimation = animation;
    animation->setDuration(DragAnimationMs);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);

    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, animation, old, target, oldBefore, oldAfter,
             newBefore, newAfter, wantedBefore, wantedAfter](const QVariant& value) {
        if (dropGapAnimation != animation) {
            return;
        }
        const qreal progress = value.toReal();
        if (old.isValid() && old != target) {
            model()->setData(old, qRound(oldBefore * (1.0 - progress)),
                             SidebarItem::DropGapBeforeRole);
            model()->setData(old, qRound(oldAfter * (1.0 - progress)),
                             SidebarItem::DropGapAfterRole);
        }
        if (target.isValid()) {
            const int before = qRound(newBefore + (wantedBefore - newBefore) * progress);
            const int afterValue = qRound(newAfter + (wantedAfter - newAfter) * progress);
            model()->setData(target, before, SidebarItem::DropGapBeforeRole);
            model()->setData(target, afterValue, SidebarItem::DropGapAfterRole);
        }
        doItemsLayout();
        viewport()->update();
    });
    connect(animation, &QVariantAnimation::finished, this,
            [this, animation, old, target] {
        if (dropGapAnimation != animation) {
            return;
        }
        if (old.isValid() && old != target) {
            model()->setData(old, 0, SidebarItem::DropGapBeforeRole);
            model()->setData(old, 0, SidebarItem::DropGapAfterRole);
        }
        dropGapAnimation = nullptr;
        animation->deleteLater();
        if (!target.isValid()) {
            dragGapIndex = QPersistentModelIndex();
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
    animateDropGap(QPersistentModelIndex(indexFromItem(gapAnchor, 0)),
                   gapAfter, draggedRowExtent);
}

void ChannelTree::clearDropGap(bool animate)
{
    if (!dragGapIndex.isValid()) {
        return;
    }
    if (animate) {
        animateDropGap(QPersistentModelIndex(), false, 0);
        return;
    }

    stopAnimation(dropGapAnimation);
    if (dragGapIndex.isValid()) {
        model()->setData(dragGapIndex, 0, SidebarItem::DropGapBeforeRole);
        model()->setData(dragGapIndex, 0, SidebarItem::DropGapAfterRole);
    }
    dragGapIndex = QPersistentModelIndex();
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
    if (dragGapIndex.isValid()) {
        model()->setData(dragGapIndex, 0, SidebarItem::DropGapBeforeRole);
        model()->setData(dragGapIndex, 0, SidebarItem::DropGapAfterRole);
    }
    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        if (index.isValid()) {
            model()->setData(index, 0.0, SidebarItem::DragCollapseRole);
        }
    }
    dragGapIndex = QPersistentModelIndex();
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
