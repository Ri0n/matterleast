from pathlib import Path

path = Path('sources/channel-tree/ChannelTreeDragVisuals.cpp')
text = path.read_text()
old = '''    // Exactly as in AnyKeep: the drag preview is now the only painted copy of
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
'''
new = '''    dragGapIndexes.clear();
    currentDragGapIndex = QPersistentModelIndex();
    currentDragGapAfter = false;
    currentDragGapExtent = 0;
    sourceDragGapIndex = originalAnchor
        ? QPersistentModelIndex(indexFromItem(originalAnchor, 0))
        : QPersistentModelIndex();
    sourceDragGapAfter = originalGapAfter;

    // The source already occupies exactly the amount of space wanted at the
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
if old not in text:
    raise SystemExit('target block not found')
path.write_text(text.replace(old, new, 1))
