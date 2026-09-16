from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]

def rd(x): return (ROOT/x).read_text()
def wr(x,t): (ROOT/x).write_text(t)
def rp(x,a,b):
    t=rd(x)
    if t.count(a)!=1: raise RuntimeError(f"{x}: anchor count {t.count(a)} for {a[:80]!r}")
    wr(x,t.replace(a,b,1))

rp("sources/channel-tree/ChannelTree.h",
   "    QPersistentModelIndex               currentDragGapIndex;\n"
   "    int                                 draggedRowExtent = 0;",
   "    QPersistentModelIndex               currentDragGapIndex;\n"
   "    bool                                currentDragGapAfter = false;\n"
   "    int                                 currentDragGapExtent = 0;\n"
   "    int                                 draggedRowExtent = 0;")

path="sources/channel-tree/ChannelTreeDragVisuals.cpp"
t=rd(path)
old='''void ChannelTree::updateDragVisuals(QTreeWidgetItem* source,
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
}'''
new='''void ChannelTree::updateDragVisuals(QTreeWidgetItem* source,
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
}'''
if old not in t: raise RuntimeError("updateDragVisuals anchor missing")
t=t.replace(old,new,1)

# Set desired boundary state once when an animation is launched.
t=t.replace("    currentDragGapIndex = target;\n\n    if (indexes.isEmpty()) {",
            "    currentDragGapIndex = target;\n"
            "    currentDragGapAfter = after;\n"
            "    currentDragGapExtent = target.isValid() ? qMax(0, extent) : 0;\n\n"
            "    if (indexes.isEmpty()) {",1)

# Do not restart an already-running close animation while the pointer remains
# outside valid targets.
t=t.replace("void ChannelTree::clearDropGap(bool animate)\n{\n    if (dragGapIndexes.isEmpty()) {\n        return;\n    }\n    if (animate) {",
            "void ChannelTree::clearDropGap(bool animate)\n{\n"
            "    if (dragGapIndexes.isEmpty()) {\n        return;\n    }\n"
            "    if (animate && !currentDragGapIndex.isValid()\n"
            "        && currentDragGapExtent == 0 && dropGapAnimation) {\n"
            "        return;\n"
            "    }\n"
            "    if (animate) {",1)

# Reset desired-state metadata on immediate clear/reset and after close finish.
t=t.replace("    dragGapIndexes.clear();\n    currentDragGapIndex = QPersistentModelIndex();\n}",
            "    dragGapIndexes.clear();\n"
            "    currentDragGapIndex = QPersistentModelIndex();\n"
            "    currentDragGapAfter = false;\n"
            "    currentDragGapExtent = 0;\n}",1)
t=t.replace("        } else {\n            currentDragGapIndex = QPersistentModelIndex();\n        }",
            "        } else {\n"
            "            currentDragGapIndex = QPersistentModelIndex();\n"
            "            currentDragGapAfter = false;\n"
            "            currentDragGapExtent = 0;\n"
            "        }",1)
t=t.replace("    dragGapIndexes.clear();\n    currentDragGapIndex = QPersistentModelIndex();\n    dragSourceIndexes.clear();",
            "    dragGapIndexes.clear();\n"
            "    currentDragGapIndex = QPersistentModelIndex();\n"
            "    currentDragGapAfter = false;\n"
            "    currentDragGapExtent = 0;\n"
            "    dragSourceIndexes.clear();",1)
wr(path,t)

# QPointer is used directly by the reconciliation TU.
rp("sources/channel-tree/ChannelTreeReconcile.cpp",
   "#include <QSet>\n",
   "#include <QPointer>\n#include <QSet>\n")

print("gap stabilization applied")
