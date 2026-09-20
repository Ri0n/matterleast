/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "ThinSplitter.h"

#include <QPainter>
#include <QPalette>
#include <QRegion>
#include <QResizeEvent>
#include <QSplitterHandle>

namespace Mattermost {
namespace {

class ThinSplitterHandle final : public QSplitterHandle
{
public:
    ThinSplitterHandle(Qt::Orientation orientation, QSplitter* parent)
        : QSplitterHandle(orientation, parent)
    {
        // QSplitter's normal tiny-handle mode grows the hit target on both
        // sides. Keep the same masked-paint / unmasked-mouse technique, but
        // make the overlap one-sided so the preceding pane remains untouched.
        setAttribute(Qt::WA_MouseNoMask, true);
        updateInteractionMargins();
    }

    QSize sizeHint() const override
    {
        // The layout must reserve exactly one pixel regardless of desktop
        // style. The extra mouse target comes exclusively from contents
        // margins that QSplitter lays over the following pane.
        return QSize(ThinSplitter::VisibleHandleExtent,
                     ThinSplitter::VisibleHandleExtent);
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        updateInteractionMargins();

        // Only the one-pixel contents rect is visible. WA_MouseNoMask makes
        // the complete (larger) handle geometry remain interactive.
        setMask(QRegion(contentsRect()));

        // Deliberately bypass QSplitterHandle::resizeEvent(): Qt's built-in
        // tiny-handle implementation would replace our asymmetric margins
        // with symmetric overlap.
        QWidget::resizeEvent(event);
    }

    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(contentsRect(), palette().color(QPalette::Mid));
    }

private:
    void updateInteractionMargins()
    {
        if (orientation() == Qt::Horizontal) {
            // QSplitter mirrors widget order in RTL layouts. Keep the overlap
            // on the logical following pane in both directions.
            if (splitter() && splitter()->isRightToLeft()) {
                setContentsMargins(ThinSplitter::TrailingHitPadding, 0, 0, 0);
            } else {
                setContentsMargins(0, 0, ThinSplitter::TrailingHitPadding, 0);
            }
        } else {
            setContentsMargins(0, 0, 0, ThinSplitter::TrailingHitPadding);
        }
    }
};

} // namespace

ThinSplitter::ThinSplitter(Qt::Orientation orientation, QWidget* parent)
    : QSplitter(orientation, parent)
{
    setHandleWidth(VisibleHandleExtent);
}

QSplitterHandle* ThinSplitter::createHandle()
{
    return new ThinSplitterHandle(orientation(), this);
}

} // namespace Mattermost
