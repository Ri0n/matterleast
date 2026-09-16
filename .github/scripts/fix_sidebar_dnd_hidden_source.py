from pathlib import Path

root = Path(__file__).resolve().parents[2]
visuals = root / "sources/channel-tree/ChannelTreeDragVisuals.cpp"
text = visuals.read_text()

old = '''    // The source already occupies exactly the amount of space wanted at the
    // original insertion boundary. There is nothing useful to animate at drag
    // start. Replace it atomically by an equal-size gap before the first
    // repaint; only subsequent boundary-to-boundary moves are animated.
    if (sourceDragGapIndex.isValid()) {
        {
            QSignalBlocker blocker(model());
            for (const QPersistentModelIndex& index : dragSourceIndexes) {
                if (!index.isValid()) {
                    continue;
                }
                model()->setData(index, true, SidebarItem::DragSourceHiddenRole);
                model()->setData(index, 1.0, SidebarItem::DragCollapseRole);
            }
            model()->setData(sourceDragGapIndex,
                             sourceDragGapAfter ? 0 : draggedRowExtent,
                             SidebarItem::DropGapBeforeRole);
            model()->setData(sourceDragGapIndex,
                             sourceDragGapAfter ? draggedRowExtent : 0,
                             SidebarItem::DropGapAfterRole);
        }
        dragGapIndexes.push_back(sourceDragGapIndex);
        currentDragGapIndex = sourceDragGapIndex;
        currentDragGapAfter = sourceDragGapAfter;
        currentDragGapExtent = draggedRowExtent;
        doItemsLayout();
        viewport()->update();
    }
'''
new = '''    // QTreeView caches delegate size hints aggressively during a drag. Trying
    // to remove the source through an animated sizeHint therefore leaves its
    // old layout extent around while the replacement gap is already visible.
    // Hide the real tree item instead: QTreeView then removes the source (and,
    // for a category, its whole subtree) from layout using its native path.
    // The equal-size initial gap keeps total geometry unchanged.
    source->setHidden(true);
    if (sourceDragGapIndex.isValid()) {
        model()->setData(sourceDragGapIndex,
                         sourceDragGapAfter ? 0 : draggedRowExtent,
                         SidebarItem::DropGapBeforeRole);
        model()->setData(sourceDragGapIndex,
                         sourceDragGapAfter ? draggedRowExtent : 0,
                         SidebarItem::DropGapAfterRole);
        dragGapIndexes.push_back(sourceDragGapIndex);
        currentDragGapIndex = sourceDragGapIndex;
        currentDragGapAfter = sourceDragGapAfter;
        currentDragGapExtent = draggedRowExtent;
    }
    doItemsLayout();
    viewport()->update();
'''
assert old in text
text = text.replace(old, new)

old = '''    QVector<qreal> sourceStarts;
    sourceStarts.reserve(dragSourceIndexes.size());
    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        sourceStarts.push_back(collapseValue(index));
    }

'''
assert old in text
text = text.replace(old, "")

old = '''    if (gapIndexes.isEmpty() && dragSourceIndexes.isEmpty()) {
        return;
    }
'''
new = '''    if (gapIndexes.isEmpty()) {
        return;
    }
'''
assert old in text
text = text.replace(old, new)

old = '''    const int wantedExtent = target.isValid() ? qMax(0, extent) : 0;
    const qreal wantedCollapse = target.isValid() ? 1.0 : 0.0;
    const auto sourceIndexes = dragSourceIndexes;

    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, animation, gapIndexes, startBefore, startAfter,
             target, after, wantedExtent, sourceIndexes, sourceStarts,
             wantedCollapse](const QVariant& value) {
'''
new = '''    const int wantedExtent = target.isValid() ? qMax(0, extent) : 0;

    connect(animation, &QVariantAnimation::valueChanged, this,
            [this, animation, gapIndexes, startBefore, startAfter,
             target, after, wantedExtent](const QVariant& value) {
'''
assert old in text
text = text.replace(old, new)

old_start = '''        // QTreeView reacts to every dataChanged signal by invalidating row
        // geometry. Updating the source collapse and destination gap one index
        // at a time therefore exposes transient frames whose total extent is
        // larger than the original tree. Apply the whole animation tick as one
        // geometry transaction and perform exactly one layout afterwards.
        {
            QSignalBlocker blocker(model());

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
'''
new_start = '''        // The source item is physically hidden from QTreeView layout. Only the
        // old and new insertion gaps need animation now; keep normal
        // dataChanged delivery so the view invalidates cached size hints.
        for (qsizetype i = 0; i < gapIndexes.size(); ++i) {
'''
assert old_start in text
text = text.replace(old_start, new_start)

old_end = '''                model()->setData(gapIndexes[i],
                                 qRound(startAfter[i]
                                        + (wantedAfter - startAfter[i]) * progress),
                                 SidebarItem::DropGapAfterRole);
            }
        }

        doItemsLayout();
'''
new_end = '''            model()->setData(gapIndexes[i],
                             qRound(startAfter[i]
                                    + (wantedAfter - startAfter[i]) * progress),
                             SidebarItem::DropGapAfterRole);
        }

        doItemsLayout();
'''
assert old_end in text
text = text.replace(old_end, new_end)

old = '''    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        if (index.isValid()) {
            model()->setData(index, 1.0, SidebarItem::DragCollapseRole);
        }
    }
    currentDragGapIndex = sourceDragGapIndex;
'''
new = '''    currentDragGapIndex = sourceDragGapIndex;
'''
assert old in text
text = text.replace(old, new)

old = '''    for (const QPersistentModelIndex& index : dragSourceIndexes) {
        if (index.isValid()) {
            model()->setData(index, 0.0, SidebarItem::DragCollapseRole);
            model()->setData(index, false, SidebarItem::DragSourceHiddenRole);
        }
    }
    dragGapIndexes.clear();
'''
new = '''    if (!dragSourceIndexes.isEmpty() && dragSourceIndexes.front().isValid()) {
        if (QTreeWidgetItem* source = itemFromIndex(dragSourceIndexes.front())) {
            source->setHidden(false);
        }
    }
    dragGapIndexes.clear();
'''
assert old in text
text = text.replace(old, new)

visuals.write_text(text)

channel_tree = root / "sources/channel-tree/ChannelTree.cpp"
text = channel_tree.read_text()
text = text.replace("    setDragDropMode(QAbstractItemView::InternalMove);\n",
                    "    setDragDropMode(QAbstractItemView::DragDrop);\n")

old = '''    const auto selected = selectedItems();
    QTreeWidgetItem* source = selected.size() == 1 ? selected.front() : currentItem();
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
'''
new = '''    QTreeWidgetItem* source = nullptr;
    if (!dragSourceIndexes.isEmpty() && dragSourceIndexes.front().isValid()) {
        source = itemFromIndex(dragSourceIndexes.front());
    }
    if (!source) {
        const auto selected = selectedItems();
        source = selected.size() == 1 ? selected.front() : currentItem();
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
'''
assert old in text
text = text.replace(old, new, 1)

old = '''    const auto selected = selectedItems();
    QTreeWidgetItem* source = selected.size() == 1 ? selected.front() : currentItem();
    const QPoint pos = dropEventPosition(event);
'''
new = '''    QTreeWidgetItem* source = nullptr;
    if (!dragSourceIndexes.isEmpty() && dragSourceIndexes.front().isValid()) {
        source = itemFromIndex(dragSourceIndexes.front());
    }
    if (!source) {
        const auto selected = selectedItems();
        source = selected.size() == 1 ? selected.front() : currentItem();
    }
    const QPoint pos = dropEventPosition(event);
'''
assert old in text
text = text.replace(old, new, 1)

channel_tree.write_text(text)
