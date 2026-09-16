from pathlib import Path

path = Path('sources/channel-tree/ChannelTree.cpp')
text = path.read_text()
old = '''void ChannelTree::dragMoveEvent(QDragMoveEvent* event)\n{\n    QTreeWidget::dragMoveEvent(event);\n    const auto selected = selectedItems();\n'''
new = '''void ChannelTree::dragMoveEvent(QDragMoveEvent* event)\n{\n    // ChannelTree owns drag target resolution and structural displacement.\n    // Calling QTreeWidget::dragMoveEvent() here would also update Qt's\n    // InternalMove state from the already modified row geometry, so even a\n    // horizontal-only pointer move could perturb the logical drop target.\n    const auto selected = selectedItems();\n'''
if old not in text:
    raise SystemExit('dragMoveEvent prologue not found')
path.write_text(text.replace(old, new, 1))
