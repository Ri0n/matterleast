#pragma once

#include <algorithm>

#include <QList>
#include <QSplitter>

namespace Mattermost {

// The right-hand pane is explicitly hidden when no docked thread is open.
// Never persist the zero width QSplitter reports for that hidden child.
inline int expandedThreadPaneWidth(const QSplitter& splitter)
{
    if (splitter.count() != 2 || !splitter.widget(1)
        || splitter.widget(1)->isHidden()) {
        return 0;
    }
    const QList<int> widths = splitter.sizes();
    return widths.size() == 2 && widths.at(0) > 0 && widths.at(1) > 0
        ? widths.at(1) : 0;
}

inline bool restoreThreadPaneWidth(QSplitter& splitter, int preferredWidth)
{
    if (preferredWidth <= 0 || splitter.count() != 2
        || !splitter.widget(1) || splitter.widget(1)->isHidden()) {
        return false;
    }
    const QList<int> widths = splitter.sizes();
    if (widths.size() != 2 || widths.at(0) + widths.at(1) <= 1) {
        return false;
    }
    const int total = widths.at(0) + widths.at(1);
    const int right = std::min(total - 1, preferredWidth);
    splitter.setSizes({total - right, right});
    return true;
}

inline bool ensureThreadPaneExpanded(QSplitter& splitter, bool forceDefaultSize = false)
{
    // QSplitter::restoreState() persists orientation as well as sizes. The
    // thread surface is semantically a right-hand pane, so never let a stale
    // vertical splitter state move it below the channel after restoration.
    if (splitter.orientation() != Qt::Horizontal) {
        splitter.setOrientation(Qt::Horizontal);
    }

    const QList<int> sizes = splitter.sizes();
    if (!forceDefaultSize && sizes.size() >= 2 && sizes.at(1) > 0) {
        return false;
    }

    const int width = std::max(900, splitter.width());
    splitter.setSizes({std::max(480, width * 2 / 3),
                       std::max(320, width / 3)});
    return true;
}

} // namespace Mattermost
