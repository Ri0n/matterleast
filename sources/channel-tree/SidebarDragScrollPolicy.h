#pragma once

#include <QPoint>
#include <QSize>
#include <QtGlobal>

namespace Mattermost {

// Scroll intent is independent from sidebar drop-target resolution. Qt's
// default item-view DnD autoscroll only runs inside the base dragMoveEvent,
// which ChannelTree intentionally bypasses to retain animated row ordering.
struct SidebarDragScrollPolicy
{
    static constexpr int EdgePixels = 40;
    static constexpr int HorizontalTolerance = 24;

    // Signed scrollbar units per timer tick. Allow moving vertically outside
    // the viewport; the pointer can be over the window frame while dragging.
    // Do not scroll when the pointer has left the sidebar horizontally.
    static int edgeStep(const QPoint& pointer, const QSize& viewport,
                        int singleStep)
    {
        if (viewport.width() <= 0 || viewport.height() <= 0
            || pointer.x() < -HorizontalTolerance
            || pointer.x() >= viewport.width() + HorizontalTolerance) {
            return 0;
        }

        const int edge = qMin(EdgePixels, qMax(1, viewport.height() / 4));
        int depth = 0;
        int direction = 0;
        if (pointer.y() < edge) {
            direction = -1;
            depth = edge - pointer.y();
        } else if (pointer.y() >= viewport.height() - edge) {
            direction = 1;
            depth = pointer.y() - (viewport.height() - edge) + 1;
        }
        if (!direction) {
            return 0;
        }

        // Prefer deliberate scrolling over the old four-row jumps:
        // one scrollbar step at the edge, two at most well outside it.
        const int acceleration = 1 + qMin(1, (depth - 1) / 40);
        return direction * qMax(1, singleStep) * acceleration;
    }

    // Horizontal wheel movement is left to Qt; vertical wheel events during
    // a drag use the same scrollbar units as normal tree scrolling.
    static int wheelStep(const QPoint& pixelDelta, const QPoint& angleDelta,
                         int singleStep, int wheelLines)
    {
        if (pixelDelta.y() != 0) {
            return -pixelDelta.y();
        }
        if (angleDelta.y() == 0) {
            return 0;
        }
        return -angleDelta.y() * qMax(1, singleStep)
            * qMax(1, wheelLines) / 120;
    }

    static int categoryProbe(int sourceLogicalY, int pointerY,
                             int startPointerY, int scrollPixels)
    {
        return sourceLogicalY + (pointerY - startPointerY) + scrollPixels;
    }

    static QPoint clampToViewport(const QPoint& pointer, const QSize& viewport)
    {
        return QPoint(qBound(0, pointer.x(), qMax(0, viewport.width() - 1)),
                      qBound(0, pointer.y(), qMax(0, viewport.height() - 1)));
    }
};

} // namespace Mattermost
