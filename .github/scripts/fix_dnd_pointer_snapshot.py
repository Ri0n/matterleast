from pathlib import Path

path = Path('sources/channel-tree/ChannelTreeDragVisuals.cpp')
text = path.read_text()
old = '''    const QPoint cursorInViewport = viewport()->mapFromGlobal(QCursor::pos());
    dragStartPointerY = cursorInViewport.y();
    draggedBlockHotSpotY = blockRect.isValid()
'''
new = '''    const QPoint cursorInViewport = viewport()->mapFromGlobal(QCursor::pos());
    draggedBlockHotSpotY = blockRect.isValid()
'''
if old not in text:
    raise SystemExit('initial pointer snapshot anchor not found')
text = text.replace(old, new, 1)
old = '''    ensureDragSourceVisuals(source);
    drag.exec(Qt::MoveAction, Qt::MoveAction);
'''
new = '''    ensureDragSourceVisuals(source);
    dragStartPointerY = cursorInViewport.y();
    drag.exec(Qt::MoveAction, Qt::MoveAction);
'''
if old not in text:
    raise SystemExit('drag exec anchor not found')
path.write_text(text.replace(old, new, 1))
