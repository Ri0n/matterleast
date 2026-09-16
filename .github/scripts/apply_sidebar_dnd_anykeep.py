from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise SystemExit(f"{label}: anchor not found")
    return text.replace(old, new, 1)


# ChannelTree.h: store the source-removed category insertion boundaries exactly
# like AnyKeep's LinearReorderLayout/GenericReorderController pair.
path = Path("sources/channel-tree/ChannelTree.h")
text = path.read_text()
old = '''    bool resolveCategoryDropTarget(QTreeWidgetItem* source, const QPoint& pos,
                                   QTreeWidgetItem*& targetCategoryItem,
                                   bool& afterTarget) const;
    void updateDragVisuals(QTreeWidgetItem* source, QTreeWidgetItem* gapAnchor,
                           bool gapAfter);
'''
new = '''    struct CategoryDragBoundary {
        int position = 0;
        QPersistentModelIndex targetCategory;
        bool afterTarget = false;
    };

    bool resolveCategoryDropTarget(QTreeWidgetItem* source, const QPoint& pos,
                                   QTreeWidgetItem*& targetCategoryItem,
                                   bool& afterTarget) const;
    void prepareCategoryDragBoundaries(QTreeWidgetItem* source);
    const CategoryDragBoundary* nearestCategoryDragBoundary(int probe) const;
    void updateDragVisuals(QTreeWidgetItem* source, QTreeWidgetItem* gapAnchor,
                           bool gapAfter);
'''
text = replace_once(text, old, new, "ChannelTree.h methods")
old = '''    int                                 currentDragGapExtent = 0;
    int                                 draggedRowExtent = 0;
    int                                 draggedBlockHotSpotY = 0;
    QMap<QString, quint64>              sidebarMutationGeneration;
'''
new = '''    int                                 currentDragGapExtent = 0;
    int                                 draggedRowExtent = 0;
    int                                 draggedBlockHotSpotY = 0;
    int                                 dragStartPointerY = 0;
    int                                 draggedBlockStartLogicalY = 0;
    QVector<CategoryDragBoundary>       categoryDragBoundaries;
    QMap<QString, quint64>              sidebarMutationGeneration;
'''
text = replace_once(text, old, new, "ChannelTree.h fields")
path.write_text(text)


# ChannelTree.cpp: category targeting is no longer based on itemAt()/block
# centres.  The probe is the dragged block's leading edge and the candidates
# are the insertion boundaries of the list after removing the source.
path = Path("sources/channel-tree/ChannelTree.cpp")
text = path.read_text()
start = text.index("QRect categoryBlockContentRect(")
end = text.index("\nbool channelDropChangesPosition(", start)
text = text[:start] + text[end + 1:]

start = text.index("bool ChannelTree::resolveCategoryDropTarget(")
end = text.index("\nvoid ChannelTree::dragMoveEvent(", start)
replacement = '''bool ChannelTree::resolveCategoryDropTarget(QTreeWidgetItem* source,
                                            const QPoint& pos,
                                            QTreeWidgetItem*& targetCategoryItem,
                                            bool& afterTarget) const
{
    targetCategoryItem = nullptr;
    afterTarget = false;
    if (!source || source->data(0, ItemKindRole).toInt() != CategoryItemKind
        || !source->parent() || categoryDragBoundaries.isEmpty()) {
        return false;
    }

    // Match AnyKeep's GenericReorderController: compare the dragged block's
    // leading edge against insertion boundaries computed after removing the
    // source.  Nearest-boundary selection means a neighbour moves once the
    // leading edge has crossed half of that neighbour's extent.  Moving up and
    // down therefore naturally use the two different half-overlap thresholds.
    const int probe = draggedBlockStartLogicalY + (pos.y() - dragStartPointerY);
    const CategoryDragBoundary* boundary = nearestCategoryDragBoundary(probe);
    if (!boundary || !boundary->targetCategory.isValid()) {
        return false;
    }

    targetCategoryItem = itemFromIndex(boundary->targetCategory);
    afterTarget = boundary->afterTarget;
    return targetCategoryItem
        && targetCategoryItem->data(0, ItemKindRole).toInt() == CategoryItemKind
        && targetCategoryItem->parent() == source->parent();
}
'''
text = text[:start] + replacement + text[end:]
path.write_text(text)


# ChannelTreeDragVisuals.cpp: AnyKeep semantics are:
#   * capture preview first;
#   * hide source immediately (opacity only, geometry unchanged at t=0);
#   * choose the original source-removed insertion boundary immediately;
#   * animate source collapse and destination gap with the same progress;
#   * keep source hidden throughout the drag and move only the gap afterward.
path = Path("sources/channel-tree/ChannelTreeDragVisuals.cpp")
text = path.read_text()
text = replace_once(
    text,
    '''    const QPoint cursorInViewport = viewport()->mapFromGlobal(QCursor::pos());
    draggedBlockHotSpotY = blockRect.isValid()
''',
    '''    const QPoint cursorInViewport = viewport()->mapFromGlobal(QCursor::pos());
    dragStartPointerY = cursorInViewport.y();
    draggedBlockHotSpotY = blockRect.isValid()
''',
    "startDrag pointer snapshot")

start = text.index("void ChannelTree::ensureDragSourceVisuals(")
end = text.index("\nvoid ChannelTree::animateSourceCollapse(", start)
ensure = r'''void ChannelTree::ensureDragSourceVisuals(QTreeWidgetItem* source)
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
'''
text = text[:start] + ensure + text[end:]

start = text.index("void ChannelTree::restoreSourceDropGap(")
end = text.index("\nvoid ChannelTree::clearDropGap(", start)
restore = r'''void ChannelTree::restoreSourceDropGap(bool animate)
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
'''
text = text[:start] + restore + text[end:]

# While a drag source exists, "clear" means return the gap to the original
# source-removed boundary; the hidden source must not expand into a second copy.
start = text.index("void ChannelTree::clearDropGap(")
end = text.index("\nvoid ChannelTree::resetDragVisuals(", start)
clear = r'''void ChannelTree::clearDropGap(bool animate)
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
'''
text = text[:start] + clear + text[end:]

# resetDragVisuals(false) restores painting as well as geometry.
needle = '''    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        if (index.isValid()) {
            model()->setData(index, 0.0, SidebarItem::DragCollapseRole);
        }
    }
'''
replacement = '''    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        if (index.isValid()) {
            model()->setData(index, 0.0, SidebarItem::DragCollapseRole);
            model()->setData(index, false, SidebarItem::DragSourceHiddenRole);
        }
    }
'''
text = replace_once(text, needle, replacement, "reset source visibility")
needle = '''    draggedRowExtent = 0;
    draggedBlockHotSpotY = 0;
    doItemsLayout();
'''
replacement = '''    draggedRowExtent = 0;
    draggedBlockHotSpotY = 0;
    dragStartPointerY = 0;
    draggedBlockStartLogicalY = 0;
    categoryDragBoundaries.clear();
    doItemsLayout();
'''
text = replace_once(text, needle, replacement, "reset boundary snapshot")
path.write_text(text)


# The previous delegate test encoded the obsolete "source owns its own gap"
# model. Replace it with the invariant we actually need: source extent can
# collapse independently while a different row carries the matching gap.
path = Path("tests/SidebarItemDelegateTest.cpp")
text = path.read_text()
old = '''    void sourcePlaceholderPreservesOriginalRowExtent()
    {
        QStandardItemModel model;
        auto* item = new QStandardItem(QStringLiteral("conversation"));
        item->setData(SidebarItem::Channel, SidebarItem::KindRole);
        model.appendRow(item);

        QListView view;
        view.setModel(&model);
        ChannelItemDelegate delegate;
        QStyleOptionViewItem option;
        option.initFrom(&view);

        const QModelIndex index = model.index(0, 0);
        const int originalHeight = delegate.sizeHint(option, index).height();
        QCOMPARE(originalHeight, 32);

        item->setData(1.0, SidebarItem::DragCollapseRole);
        item->setData(originalHeight, SidebarItem::DropGapBeforeRole);
        QCOMPARE(delegate.sizeHint(option, index).height(), originalHeight);

        item->setData(0, SidebarItem::DropGapBeforeRole);
        QCOMPARE(delegate.sizeHint(option, index).height(), 0);
    }
'''
new = '''    void sourceCollapseAndExternalGapConserveExtent()
    {
        QStandardItemModel model;
        auto* source = new QStandardItem(QStringLiteral("source"));
        source->setData(SidebarItem::Channel, SidebarItem::KindRole);
        auto* target = new QStandardItem(QStringLiteral("target"));
        target->setData(SidebarItem::Channel, SidebarItem::KindRole);
        model.appendRow(source);
        model.appendRow(target);

        QListView view;
        view.setModel(&model);
        ChannelItemDelegate delegate;
        QStyleOptionViewItem option;
        option.initFrom(&view);

        const QModelIndex sourceIndex = model.index(0, 0);
        const QModelIndex targetIndex = model.index(1, 0);
        QCOMPARE(delegate.sizeHint(option, sourceIndex).height(), 32);
        QCOMPARE(delegate.sizeHint(option, targetIndex).height(), 32);

        for (int step = 0; step <= 4; ++step) {
            const qreal progress = step / 4.0;
            source->setData(progress, SidebarItem::DragCollapseRole);
            target->setData(qRound(32 * progress), SidebarItem::DropGapBeforeRole);
            QCOMPARE(delegate.sizeHint(option, sourceIndex).height()
                         + delegate.sizeHint(option, targetIndex).height(),
                     64);
        }

        source->setData(true, SidebarItem::DragSourceHiddenRole);
        QCOMPARE(delegate.sizeHint(option, sourceIndex).height(), 0);
        QCOMPARE(delegate.sizeHint(option, targetIndex).height(), 64);
    }
'''
text = replace_once(text, old, new, "SidebarItemDelegateTest old placeholder test")
path.write_text(text)
