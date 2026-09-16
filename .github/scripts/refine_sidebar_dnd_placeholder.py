from pathlib import Path

root = Path(__file__).resolve().parents[2]

# ChannelTree.h
path = root / 'sources/channel-tree/ChannelTree.h'
text = path.read_text()
old = '''    void updateDragVisuals(QTreeWidgetItem* source, QTreeWidgetItem* gapAnchor,
                           bool gapAfter);
    void clearDropGap(bool animate);
    void resetDragVisuals(bool animate);
'''
new = '''    void updateDragVisuals(QTreeWidgetItem* source, QTreeWidgetItem* gapAnchor,
                           bool gapAfter);
    void restoreSourceDropGap(bool animate);
    void clearDropGap(bool animate);
    void resetDragVisuals(bool animate);
'''
if old not in text:
    raise SystemExit('ChannelTree.h method anchor not found')
text = text.replace(old, new, 1)
old = '''    int                                 currentDragGapExtent = 0;
    int                                 draggedRowExtent = 0;
    QMap<QString, quint64>              sidebarMutationGeneration;
'''
new = '''    int                                 currentDragGapExtent = 0;
    int                                 draggedRowExtent = 0;
    int                                 draggedBlockHotSpotY = 0;
    QMap<QString, quint64>              sidebarMutationGeneration;
'''
if old not in text:
    raise SystemExit('ChannelTree.h field anchor not found')
path.write_text(text.replace(old, new, 1))

# ChannelTreeDragVisuals.cpp
path = root / 'sources/channel-tree/ChannelTreeDragVisuals.cpp'
text = path.read_text()
old = '''    const QRect previewRect = blockRect.intersected(viewport()->rect());
    QDrag drag(this);
'''
new = '''    const QPoint cursorInViewport = viewport()->mapFromGlobal(QCursor::pos());
    draggedBlockHotSpotY = blockRect.isValid()
        ? qBound(0, cursorInViewport.y() - blockRect.top(), qMax(0, blockRect.height() - 1))
        : 0;

    const QRect previewRect = blockRect.intersected(viewport()->rect());
    QDrag drag(this);
'''
if old not in text:
    raise SystemExit('drag preview anchor not found')
text = text.replace(old, new, 1)
old = '''        QPoint hotSpot = viewport()->mapFromGlobal(QCursor::pos()) - previewRect.topLeft();
'''
new = '''        QPoint hotSpot = cursorInViewport - previewRect.topLeft();
'''
if old not in text:
    raise SystemExit('drag hotspot anchor not found')
text = text.replace(old, new, 1)
old = '''    ensureDragSourceVisuals(source);
    const Qt::DropAction result = drag.exec(Qt::MoveAction, Qt::MoveAction);
    if (!dragSourceIndexes.isEmpty() || !dragGapIndexes.isEmpty()) {
        resetDragVisuals(result == Qt::IgnoreAction);
    }
'''
new = '''    ensureDragSourceVisuals(source);
    drag.exec(Qt::MoveAction, Qt::MoveAction);
    if (!dragSourceIndexes.isEmpty() || !dragGapIndexes.isEmpty()) {
        resetDragVisuals(false);
    }
'''
if old not in text:
    raise SystemExit('drag exec anchor not found')
text = text.replace(old, new, 1)
old = '''void ChannelTree::dragLeaveEvent(QDragLeaveEvent* event)
{
    QTreeWidget::dragLeaveEvent(event);
    clearDropGap(true);
}
'''
new = '''void ChannelTree::dragLeaveEvent(QDragLeaveEvent* event)
{
    QTreeWidget::dragLeaveEvent(event);
    restoreSourceDropGap(true);
}
'''
if old not in text:
    raise SystemExit('dragLeaveEvent anchor not found')
text = text.replace(old, new, 1)
old = '''    if (draggedRowExtent <= 0) {
        draggedRowExtent = visualItemRect(source).height();
    }
    animateSourceCollapse(1.0);
}
'''
new = '''    if (draggedRowExtent <= 0) {
        draggedRowExtent = visualItemRect(source).height();
    }

    // Replace the dragged block by an equal-sized structural placeholder in a
    // single layout pass. Starting a drag must not move anything below source.
    // Only this placeholder travels through the tree afterwards.
    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        if (index.isValid()) {
            model()->setData(index, 1.0, SidebarItem::DragCollapseRole);
        }
    }
    const QPersistentModelIndex placeholder = dragSourceIndexes.front();
    dragGapIndexes.clear();
    dragGapIndexes.push_back(placeholder);
    currentDragGapIndex = placeholder;
    currentDragGapAfter = false;
    currentDragGapExtent = draggedRowExtent;
    model()->setData(placeholder, draggedRowExtent, SidebarItem::DropGapBeforeRole);
    model()->setData(placeholder, 0, SidebarItem::DropGapAfterRole);
    doItemsLayout();
    viewport()->update();
}
'''
if old not in text:
    raise SystemExit('ensureDragSourceVisuals tail not found')
text = text.replace(old, new, 1)
anchor = '''void ChannelTree::clearDropGap(bool animate)
{
'''
if anchor not in text:
    raise SystemExit('clearDropGap anchor not found')
helper = '''void ChannelTree::restoreSourceDropGap(bool animate)
{
    if (dragSourceIndexes.isEmpty() || draggedRowExtent <= 0
        || !dragSourceIndexes.front().isValid()) {
        return;
    }

    const QPersistentModelIndex source = dragSourceIndexes.front();
    if (currentDragGapIndex == source && !currentDragGapAfter
        && currentDragGapExtent == draggedRowExtent) {
        return;
    }
    if (animate) {
        animateDropGap(source, false, draggedRowExtent);
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
    dragGapIndexes.push_back(source);
    model()->setData(source, draggedRowExtent, SidebarItem::DropGapBeforeRole);
    currentDragGapIndex = source;
    currentDragGapAfter = false;
    currentDragGapExtent = draggedRowExtent;
    doItemsLayout();
    viewport()->update();
}

'''
text = text.replace(anchor, helper + anchor, 1)
old = '''    dragSourceIndexes.clear();
    draggedRowExtent = 0;
    doItemsLayout();
'''
new = '''    dragSourceIndexes.clear();
    draggedRowExtent = 0;
    draggedBlockHotSpotY = 0;
    doItemsLayout();
'''
if old not in text:
    raise SystemExit('reset tail anchor not found')
path.write_text(text.replace(old, new, 1))

# ChannelTree.cpp
path = root / 'sources/channel-tree/ChannelTree.cpp'
text = path.read_text()
anchor = '''bool channelDropChangesPosition(QTreeWidgetItem* source,
'''
if anchor not in text:
    raise SystemExit('ChannelTree.cpp helper anchor not found')
helper = '''QRect categoryBlockContentRect(const ChannelTree* tree, QTreeWidgetItem* category)
{
    if (!tree || !category) {
        return {};
    }

    QRect block = dragContentRect(category, tree->visualItemRect(category));
    if (!category->isExpanded()) {
        return block;
    }
    for (int i = 0; i < category->childCount(); ++i) {
        QTreeWidgetItem* child = category->child(i);
        if (!child || child->isHidden()) {
            continue;
        }
        const QRect childRect = dragContentRect(child, tree->visualItemRect(child));
        if (!childRect.isValid() || childRect.height() <= 0) {
            continue;
        }
        block = block.isValid() ? block.united(childRect) : childRect;
    }
    return block;
}

'''
text = text.replace(anchor, helper + anchor, 1)

start = text.index('bool ChannelTree::resolveCategoryDropTarget(')
end = text.index('\nvoid ChannelTree::dragMoveEvent(', start)
replacement = '''bool ChannelTree::resolveCategoryDropTarget(QTreeWidgetItem* source,
                                            const QPoint& pos,
                                            QTreeWidgetItem*& targetCategoryItem,
                                            bool& afterTarget) const
{
    targetCategoryItem = nullptr;
    afterTarget = false;
    if (!source || source->data(0, ItemKindRole).toInt() != CategoryItemKind
        || !source->parent() || draggedRowExtent <= 0) {
        return false;
    }

    // Sortable-list semantics: a neighbouring group starts moving when the
    // centre of the dragged whole group crosses the centre of that whole group.
    const int draggedCenterY = pos.y() - draggedBlockHotSpotY + draggedRowExtent / 2;
    QTreeWidgetItem* teamItem = source->parent();
    QTreeWidgetItem* lastCategory = nullptr;
    for (int i = 0; i < teamItem->childCount(); ++i) {
        QTreeWidgetItem* category = teamItem->child(i);
        if (!category || category == source
            || category->data(0, ItemKindRole).toInt() != CategoryItemKind
            || category->isHidden()) {
            continue;
        }

        const QRect block = categoryBlockContentRect(this, category);
        if (!block.isValid() || block.height() <= 0) {
            continue;
        }
        lastCategory = category;
        if (draggedCenterY < block.center().y()) {
            targetCategoryItem = category;
            afterTarget = false;
            return true;
        }
    }

    if (lastCategory) {
        targetCategoryItem = lastCategory;
        afterTarget = true;
        return true;
    }
    return false;
}
'''
text = text[:start] + replacement + text[end:]

text = text.replace('''            clearDropGap(true);
            event->ignore();
            return;
''', '''            restoreSourceDropGap(true);
            event->ignore();
            return;
''', 1)
text = text.replace('''            ensureDragSourceVisuals(source);
            clearDropGap(true);
            event->setDropAction(Qt::MoveAction);
''', '''            ensureDragSourceVisuals(source);
            restoreSourceDropGap(true);
            event->setDropAction(Qt::MoveAction);
''', 1)
text = text.replace('''        clearDropGap(true);
        event->ignore();
        return;
''', '''        restoreSourceDropGap(true);
        event->ignore();
        return;
''', 1)
text = text.replace('''        ensureDragSourceVisuals(source);
        clearDropGap(true);
        event->setDropAction(Qt::MoveAction);
''', '''        ensureDragSourceVisuals(source);
        restoreSourceDropGap(true);
        event->setDropAction(Qt::MoveAction);
''', 1)

# No-op/invalid drops simply restore the real source row atomically.
text = text.replace('resetDragVisuals(true);\n            event->ignore();',
                    'resetDragVisuals(false);\n            event->ignore();')
text = text.replace('resetDragVisuals(true);\n            event->setDropAction(Qt::MoveAction);',
                    'resetDragVisuals(false);\n            event->setDropAction(Qt::MoveAction);')
text = text.replace('resetDragVisuals(true);\n        event->ignore();',
                    'resetDragVisuals(false);\n        event->ignore();')
text = text.replace('resetDragVisuals(true);\n        event->setDropAction(Qt::MoveAction);',
                    'resetDragVisuals(false);\n        event->setDropAction(Qt::MoveAction);')
path.write_text(text)
