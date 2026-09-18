#pragma once

#include <QRect>
#include <Qt>

namespace Mattermost {

constexpr int SplitterVisibleDividerExtent = 1;

inline QRect splitterDividerRect(const QRect& handleRect, Qt::Orientation orientation)
{
    if (handleRect.isEmpty()) {
        return {};
    }

    if (orientation == Qt::Horizontal) {
        const int x = handleRect.left()
            + (handleRect.width() - SplitterVisibleDividerExtent) / 2;
        return QRect(x,
                     handleRect.top(),
                     SplitterVisibleDividerExtent,
                     handleRect.height());
    }

    const int y = handleRect.top()
        + (handleRect.height() - SplitterVisibleDividerExtent) / 2;
    return QRect(handleRect.left(),
                 y,
                 handleRect.width(),
                 SplitterVisibleDividerExtent);
}

} // namespace Mattermost
