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
#include <QCursor>
#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QToolButton>
#include <QWindow>

#include "post/PostWidget.h"
#include "ui/OverlayScrollBarManager.h"

namespace Mattermost {
namespace {

bool isFloatingPostAction(const QWidget* widget)
{
    for (const QWidget* current = widget; current;
         current = current->parentWidget()) {
        if (current->objectName() == QLatin1String("postHoverActions")) {
            return true;
        }
    }
    return false;
}

QToolButton* visibleNavigationButtonAt(QWidget* area,
                                       const QPoint& globalPosition)
{
    if (!area) {
        return nullptr;
    }
    for (const char* name : {"mattermostOverlayScrollToStartButton",
                             "mattermostOverlayScrollToEndButton"}) {
        auto* button = area->findChild<QToolButton*>(
            QString::fromLatin1(name), Qt::FindDirectChildrenOnly);
        if (button && button->isVisible() && button->isEnabled()
            && button->rect().contains(button->mapFromGlobal(globalPosition))) {
            return button;
        }
    }
    return nullptr;
}

QPoint eventGlobalPosition(QEvent* event)
{
    if (event->type() == QEvent::MouseMove
        || event->type() == QEvent::MouseButtonPress
        || event->type() == QEvent::MouseButtonRelease) {
        const auto* mouse = static_cast<QMouseEvent*>(event);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        return mouse->globalPosition().toPoint();
#else
        return mouse->screenPos().toPoint();
#endif
    }
    return QCursor::pos();
}

} // namespace

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

    // Row Enter/Leave does not cover the floating post toolbar, which is a
    // sibling of the row, or the edge navigation controls, which are siblings
    // of the viewport. Observe pointer movement throughout this list so the
    // hover panel can disappear before it covers an edge button's click.
    viewport()->installEventFilter(this);
    qApp->installEventFilter(this);

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

        if (navigationHoverSuppressed_
            || OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(
                *this, QCursor::pos())) {
            navigationHoverSuppressed_ = true;
            setHoverHighlightSuppressed(true);
            clearActiveHover(true);
            return;
        }

        if (!current) {
            // A null row inside the viewport is only a gap between
            // materialized posts or the floating action toolbar.
            return;
        }
        activateHoverPost(current);
    });
}

void PostListWidget::activateHoverPost(PostWidget* current, bool immediate)
{
    if (!current) {
        return;
    }

    setHoverHighlightOverride(current);
    const bool handoff = hoverSessionHasPost_
        && activeHoverPost_.data() != current;

    if (auto* previous = qobject_cast<PostWidget*>(activeHoverPost_.data())) {
        if (previous != current) {
            previous->setHovered(false, true);
        }
    }

    current->setHovered(true, immediate || handoff);
    activeHoverPost_ = current;
    hoverSessionHasPost_ = true;
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

PostWidget* PostListWidget::postAtPointer(
    QWidget* receiver, const QPoint& globalPosition) const
{
    if (!receiver || (receiver != viewport()
                      && !viewport()->isAncestorOf(receiver))) {
        return nullptr;
    }

    QWidget* hit = receiver == viewport()
        ? viewport()->childAt(viewport()->mapFromGlobal(globalPosition))
        : receiver;
    for (QWidget* current = hit; current && current != viewport();
         current = current->parentWidget()) {
        if (auto* post = qobject_cast<PostWidget*>(current)) {
            return post;
        }
    }
    return nullptr;
}

void PostListWidget::handlePointerOverNavigation(
    const QPoint& globalPosition, QWidget* receiver)
{
    const bool excluded =
        OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(
            *this, globalPosition);
    if (excluded) {
        navigationHoverSuppressed_ = true;
        setHoverHighlightSuppressed(true);
        // No 160ms leave delay or fade: the toolbar must stop intercepting
        // clicks before the pointer reaches the quick-scroll button.
        clearActiveHover(true);
        return;
    }

    if (!navigationHoverSuppressed_) {
        return;
    }

    navigationHoverSuppressed_ = false;
    setHoverHighlightSuppressed(false);
    // A row may never receive a fresh Enter when the pointer merely crosses
    // this invisible exclusion contour while remaining inside that row.
    if (auto* post = postAtPointer(receiver, globalPosition)) {
        activateHoverPost(post, true);
    }
}

bool PostListWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (!event) {
        return LongListWidget::eventFilter(watched, event);
    }

    auto* receiver = qobject_cast<QWidget*>(watched);
    if (event->type() == QEvent::MouseButtonRelease && interceptedEdgeButton_) {
        QPointer<QToolButton> button = interceptedEdgeButton_;
        interceptedEdgeButton_.clear();
        const auto* mouse = static_cast<QMouseEvent*>(event);
        // If a pre-existing toolbar captured the press before being hidden,
        // finish that one physical click on the underlying edge button. Never
        // propagate the release to a reaction, reply or more-actions button.
        if (mouse->button() == Qt::LeftButton && button
            && button->isVisible() && button->isEnabled()
            && button->rect().contains(
                button->mapFromGlobal(eventGlobalPosition(event)))) {
            button->click();
        }
        return true;
    }

    if (receiver && (receiver == this || isAncestorOf(receiver))) {
        switch (event->type()) {
        case QEvent::Enter:
        case QEvent::HoverMove:
        case QEvent::MouseMove:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease: {
            const QPoint global = eventGlobalPosition(event);
            handlePointerOverNavigation(global, receiver);
            // A queued Enter can still be delivered to the old floating
            // toolbar after the row was suppressed. Prevent PostWidget's
            // toolbar/quick-reaction Enter handlers from reopening it.
            if (navigationHoverSuppressed_
                && isFloatingPostAction(receiver)
                && (event->type() == QEvent::Enter
                    || event->type() == QEvent::HoverMove
                    || event->type() == QEvent::MouseMove)) {
                return true;
            }
            if ((event->type() == QEvent::MouseButtonPress
                 || event->type() == QEvent::MouseButtonRelease)
                && navigationHoverSuppressed_
                && isFloatingPostAction(receiver)) {
                if (event->type() == QEvent::MouseButtonPress) {
                    const auto* mouse = static_cast<QMouseEvent*>(event);
                    if (mouse->button() == Qt::LeftButton) {
                        interceptedEdgeButton_ = visibleNavigationButtonAt(
                            this, global);
                    }
                }
                return true;
            }
            break;
        }
        default:
            break;
        }
    }

    if (watched == viewport()) {
        if (event->type() == QEvent::Enter) {
            clearActiveHover(true);
        } else if (event->type() == QEvent::Leave) {
            // A viewport Leave cannot be a transition to the floating
            // toolbar: that toolbar lives inside the same viewport.
            clearActiveHover(true);
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
