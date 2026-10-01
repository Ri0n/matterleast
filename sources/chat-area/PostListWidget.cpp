/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 *
 * MatterLeast is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "PostListWidget.h"

#include <QApplication>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>

#include "post/PostWidget.h"

namespace Mattermost {

PostListWidget::PostListWidget(QWidget* parent)
    : LongListWidget(parent)
{
    setFrameShape(QFrame::NoFrame);
    setLineWidth(0);
    setMidLineWidth(0);
    setViewportMargins(0, 0, 0, 0);

    setMaterializationLimit(200);
    setRequestBlockSize(10);
    setPrefetchScreens(0);
    setSeekDebounceMs(100);

    // Row Enter/Leave events are not sufficient to identify a semantic hover
    // handoff: the pointer may cross a few pixels of layout gap between posts.
    // Track the viewport lifetime as the enclosing hover session instead.
    viewport()->installEventFilter(this);

    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] {
        auto* active = qobject_cast<PostWidget*>(activeHoverPost_.data());
        if (active && active->window() && !active->window()->isActiveWindow()) {
            clearActiveHover(true);
        }
    });

    connect(this, &LongListWidget::hoveredItemChanged, this,
            [this](int, int currentIndex) {
        auto* current = currentIndex >= 0
            ? qobject_cast<PostWidget*>(itemWidget(currentIndex))
            : nullptr;

        if (!current) {
            // A null row while the pointer is still inside the viewport is only
            // a gap between materialized posts or the floating toolbar itself.
            // Keep both the toolbar and row highlight attached to the previous
            // semantic post.
            return;
        }

        setHoverHighlightOverride(current);

        const bool handoff = hoverSessionHasPost_
            && activeHoverPost_.data() != current;

        if (auto* previous =
                qobject_cast<PostWidget*>(activeHoverPost_.data())) {
            if (previous != current) {
                previous->setHovered(false, true);
            }
        }

        current->setHovered(true, handoff);
        activeHoverPost_ = current;
        hoverSessionHasPost_ = true;
    });
}

void PostListWidget::clearActiveHover(bool immediate)
{
    setHoverHighlightOverride(nullptr);
    if (auto* active = qobject_cast<PostWidget*>(activeHoverPost_.data())) {
        active->setHovered(false, immediate);
    }
    activeHoverPost_.clear();
    hoverSessionHasPost_ = false;
}

bool PostListWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == viewport() && event) {
        if (event->type() == QEvent::Enter) {
            // Some window systems do not deliver the matching Leave when the
            // application loses activation. Never forget an old hover owner
            // without first hiding its viewport-owned floating toolbar.
            clearActiveHover(true);
        } else if (event->type() == QEvent::Leave) {
            clearActiveHover();
        }
    }

    return LongListWidget::eventFilter(watched, event);
}

QString PostListWidget::itemIdentity(const QWidget* widget) const
{
    const auto* postWidget = qobject_cast<const PostWidget*>(widget);
    return postWidget ? postWidget->post.id : QString();
}

} // namespace Mattermost
