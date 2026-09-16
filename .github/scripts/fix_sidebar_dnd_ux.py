from pathlib import Path

root = Path(__file__).resolve().parents[2]

# 1. Own the drag lifecycle instead of QTreeWidget::startDrag(). The base
# InternalMove implementation removes the source row after our custom dropEvent
# has already performed the optimistic move. Also build a subtree drag preview.
p = root / 'sources/channel-tree/ChannelTreeDragVisuals.cpp'
s = p.read_text()
s = s.replace('#include <QDragLeaveEvent>\n#include <QEasingCurve>\n#include <QVariantAnimation>\n',
'''#include <QCursor>\n#include <QDrag>\n#include <QDragLeaveEvent>\n#include <QEasingCurve>\n#include <QMimeData>\n#include <QVariantAnimation>\n''')
old = '''void ChannelTree::startDrag(Qt::DropActions supportedActions)\n{\n    QTreeWidget::startDrag(supportedActions);\n    if (!dragSourceIndexes.isEmpty() || !dragGapIndexes.isEmpty()) {\n        resetDragVisuals(true);\n    }\n}\n'''
new = '''void ChannelTree::startDrag(Qt::DropActions supportedActions)\n{\n    if (!(supportedActions & Qt::MoveAction)) {\n        return;\n    }\n\n    const auto selected = selectedItems();\n    QTreeWidgetItem* source = selected.size() == 1 ? selected.front() : currentItem();\n    if (!source) {\n        return;\n    }\n    const int kind = source->data(0, ItemKindRole).toInt();\n    if (kind != ChannelItemKind && kind != CategoryItemKind) {\n        return;\n    }\n\n    const QModelIndex sourceIndex = indexFromItem(source, 0);\n    if (!sourceIndex.isValid()) {\n        return;\n    }\n\n    QModelIndexList indexes;\n    indexes.push_back(sourceIndex);\n    QMimeData* mimeData = model()->mimeData(indexes);\n    if (!mimeData) {\n        return;\n    }\n\n    // Capture the drag image before collapsing the source geometry. For an\n    // expanded category the logical dragged block is the category plus all of\n    // its children; the preview shows the visible portion of that subtree.\n    QRect blockRect = visualItemRect(source);\n    if (kind == CategoryItemKind && source->isExpanded()) {\n        for (int i = 0; i < source->childCount(); ++i) {\n            QTreeWidgetItem* child = source->child(i);\n            if (!child || child->isHidden()) {\n                continue;\n            }\n            const QRect childRect = visualItemRect(child);\n            if (childRect.isValid() && childRect.height() > 0) {\n                blockRect = blockRect.united(childRect);\n            }\n        }\n    }\n\n    const QRect previewRect = blockRect.intersected(viewport()->rect());\n    QDrag drag(this);\n    drag.setMimeData(mimeData);\n    if (previewRect.isValid() && !previewRect.isEmpty()) {\n        drag.setPixmap(viewport()->grab(previewRect));\n        QPoint hotSpot = viewport()->mapFromGlobal(QCursor::pos()) - previewRect.topLeft();\n        hotSpot.setX(qBound(0, hotSpot.x(), previewRect.width() - 1));\n        hotSpot.setY(qBound(0, hotSpot.y(), previewRect.height() - 1));\n        drag.setHotSpot(hotSpot);\n    }\n\n    ensureDragSourceVisuals(source);\n    const Qt::DropAction result = drag.exec(Qt::MoveAction, Qt::MoveAction);\n    if (!dragSourceIndexes.isEmpty() || !dragGapIndexes.isEmpty()) {\n        resetDragVisuals(result == Qt::IgnoreAction);\n    }\n}\n'''
if old not in s:
    raise SystemExit('startDrag block not found')
s = s.replace(old, new)
p.write_text(s)

# 2. The animated gap is the insertion affordance; do not draw an extra line.
p = root / 'sources/channel-tree/ChannelItemDelegate.cpp'
s = p.read_text()
start = s.find('void drawGapMarker(')
end = s.find('QIcon savedDestinationIcon', start)
if start < 0 or end < 0:
    raise SystemExit('drawGapMarker block not found')
s = s[:start] + s[end:]
s = s.replace('''    if (content.rect.height() <= 0) {\n        drawGapMarker(painter, option, index);\n        return;\n    }\n''', '''    if (content.rect.height() <= 0) {\n        return;\n    }\n''')
s = s.replace('''        QStyledItemDelegate::paint(painter, content, index);\n        painter->restore();\n        drawGapMarker(painter, option, index);\n        return;\n''', '''        QStyledItemDelegate::paint(painter, content, index);\n        painter->restore();\n        return;\n''')
s = s.replace('''    painter->restore();\n    drawGapMarker(painter, option, index);\n}\n''', '''    painter->restore();\n}\n''')
if 'drawGapMarker' in s:
    raise SystemExit('drawGapMarker reference remains')
p.write_text(s)

# 3. Do not open a destination gap for the source's existing logical boundary.
# After source collapse, e.g. dropping S after its previous sibling or before its
# next sibling is a no-op; opening a full-size gap there causes the double-space
# jump reported at drag start.
p = root / 'sources/channel-tree/ChannelTree.cpp'
s = p.read_text()
needle = '''QRect dragContentRect(const QTreeWidgetItem* item, QRect rect)\n{\n    if (!item) {\n        return rect;\n    }\n    rect.adjust(0, qMax(0, item->data(0, SidebarItem::DropGapBeforeRole).toInt()),\n                0, -qMax(0, item->data(0, SidebarItem::DropGapAfterRole).toInt()));\n    return rect;\n}\n'''
insert = needle + '''\nbool channelDropChangesPosition(QTreeWidgetItem* source,\n                                QTreeWidgetItem* targetCategory,\n                                const QString& targetChannelId,\n                                bool afterTarget)\n{\n    if (!source || !targetCategory) {\n        return false;\n    }\n    if (source->parent() != targetCategory) {\n        return true;\n    }\n\n    QStringList channelIds;\n    for (int i = 0; i < targetCategory->childCount(); ++i) {\n        QTreeWidgetItem* row = targetCategory->child(i);\n        if (row && row->data(0, SidebarItem::KindRole).toInt() == SidebarItem::Channel) {\n            channelIds.push_back(row->data(0, SidebarItem::IdRole).toString());\n        }\n    }\n    return reorderSidebarChannel(channelIds,\n                                 source->data(0, SidebarItem::IdRole).toString(),\n                                 targetChannelId, afterTarget);\n}\n\nbool categoryDropChangesPosition(QTreeWidgetItem* source,\n                                 QTreeWidgetItem* targetCategory,\n                                 bool afterTarget)\n{\n    if (!source || !targetCategory || source->parent() != targetCategory->parent()) {\n        return false;\n    }\n\n    QTreeWidgetItem* team = source->parent();\n    QStringList categoryIds;\n    for (int i = 0; team && i < team->childCount(); ++i) {\n        QTreeWidgetItem* row = team->child(i);\n        if (row && row->data(0, SidebarItem::KindRole).toInt() == SidebarItem::Category) {\n            categoryIds.push_back(row->data(0, SidebarItem::IdRole).toString());\n        }\n    }\n    return reorderSidebarCategory(categoryIds,\n                                  source->data(0, SidebarItem::IdRole).toString(),\n                                  targetCategory->data(0, SidebarItem::IdRole).toString(),\n                                  afterTarget);\n}\n'''
if needle not in s:
    raise SystemExit('dragContentRect block not found')
s = s.replace(needle, insert, 1)

old = '''        bool gapAfter = afterTarget;\n        QTreeWidgetItem* anchor = categoryDropGapAnchor(\n            targetCategoryItem, afterTarget, gapAfter);\n        updateDragVisuals(source, anchor, gapAfter);\n        event->setDropAction(Qt::MoveAction);\n        event->accept();\n        return;\n'''
new = '''        if (!categoryDropChangesPosition(source, targetCategoryItem, afterTarget)) {\n            ensureDragSourceVisuals(source);\n            clearDropGap(true);\n            event->setDropAction(Qt::MoveAction);\n            event->accept();\n            return;\n        }\n        bool gapAfter = afterTarget;\n        QTreeWidgetItem* anchor = categoryDropGapAnchor(\n            targetCategoryItem, afterTarget, gapAfter);\n        updateDragVisuals(source, anchor, gapAfter);\n        event->setDropAction(Qt::MoveAction);\n        event->accept();\n        return;\n'''
if old not in s:
    raise SystemExit('category dragMove block not found')
s = s.replace(old, new, 1)

old = '''    bool gapAfter = afterTarget;\n    QTreeWidgetItem* anchor = channelDropGapAnchor(\n        source, targetCategoryItem, targetChannelId, afterTarget, gapAfter);\n    updateDragVisuals(source, anchor, gapAfter);\n    event->setDropAction(Qt::MoveAction);\n    event->accept();\n}\n'''
new = '''    if (!channelDropChangesPosition(source, targetCategoryItem,\n                                    targetChannelId, afterTarget)) {\n        ensureDragSourceVisuals(source);\n        clearDropGap(true);\n        event->setDropAction(Qt::MoveAction);\n        event->accept();\n        return;\n    }\n    bool gapAfter = afterTarget;\n    QTreeWidgetItem* anchor = channelDropGapAnchor(\n        source, targetCategoryItem, targetChannelId, afterTarget, gapAfter);\n    updateDragVisuals(source, anchor, gapAfter);\n    event->setDropAction(Qt::MoveAction);\n    event->accept();\n}\n'''
if old not in s:
    raise SystemExit('channel dragMove block not found')
s = s.replace(old, new, 1)

old = '''        const QString targetCategoryId = targetCategoryItem->data(0, ItemIdRole).toString();\n        resetDragVisuals(false);\n        moveCategory(source, targetCategoryId, afterTarget);\n'''
new = '''        const QString targetCategoryId = targetCategoryItem->data(0, ItemIdRole).toString();\n        if (!categoryDropChangesPosition(source, targetCategoryItem, afterTarget)) {\n            resetDragVisuals(true);\n            event->setDropAction(Qt::MoveAction);\n            event->accept();\n            return;\n        }\n        resetDragVisuals(false);\n        moveCategory(source, targetCategoryId, afterTarget);\n'''
if old not in s:
    raise SystemExit('category drop block not found')
s = s.replace(old, new, 1)

old = '''    auto* channelItem = static_cast<ChannelItem*>(source);\n    const QString targetCategoryId = targetCategoryItem->data(0, ItemIdRole).toString();\n    resetDragVisuals(false);\n    moveChannel(channelItem, targetCategoryId, targetChannelId,\n                afterTarget, !targetChannelId.isEmpty());\n'''
new = '''    auto* channelItem = static_cast<ChannelItem*>(source);\n    const QString targetCategoryId = targetCategoryItem->data(0, ItemIdRole).toString();\n    if (!channelDropChangesPosition(source, targetCategoryItem,\n                                    targetChannelId, afterTarget)) {\n        resetDragVisuals(true);\n        event->setDropAction(Qt::MoveAction);\n        event->accept();\n        return;\n    }\n    resetDragVisuals(false);\n    moveChannel(channelItem, targetCategoryId, targetChannelId,\n                afterTarget, !targetChannelId.isEmpty());\n'''
if old not in s:
    raise SystemExit('channel drop block not found')
s = s.replace(old, new, 1)
p.write_text(s)
