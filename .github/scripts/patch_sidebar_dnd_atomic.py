from pathlib import Path

path = Path('sources/channel-tree/ChannelTreeDragVisuals.cpp')
text = path.read_text()

text = text.replace('#include <QMimeData>\n#include <QVariantAnimation>',
                    '#include <QMimeData>\n#include <QSignalBlocker>\n#include <QVariantAnimation>')

old = '''        const qreal progress = value.toDouble();

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
'''

new = '''        const qreal progress = value.toDouble();

        // QTreeView reacts to every dataChanged signal by invalidating row
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
        }

        doItemsLayout();
        viewport()->update();
'''

if old not in text:
    raise SystemExit('animateDropGap update block not found')
text = text.replace(old, new, 1)
path.write_text(text)
