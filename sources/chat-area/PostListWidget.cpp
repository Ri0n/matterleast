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

#include <algorithm>

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPalette>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>
#include <QVariant>
#include <QWindow>

#include "backend/Backend.h"
#include "chat-area/ChatArea.h"
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"
#include "post/PostWidget.h"
#include "ui/IconUtils.h"
#include "ui/OverlayScrollBarManager.h"

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
        if (toolbarPost_ && toolbarPost_->window()
            && !toolbarPost_->window()->isActiveWindow()) {
            hideToolbar(true);
        }
    });

    connect(this, &LongListWidget::hoveredItemChanged, this,
            [this](int, int currentIndex) {
        QWidget* row = currentIndex >= 0 ? itemWidget(currentIndex) : nullptr;
        auto* current = qobject_cast<PostWidget*>(row);
        if (!current && row) {
            // Saved/search/pinned collections materialize a wrapper row whose
            // child is the actual PostWidget. Their hover target is still that
            // post, not the extra layout container.
            current = row->findChild<PostWidget*>(QString(),
                                                  Qt::FindDirectChildrenOnly);
        }

        if (!current) {
            // A null row while the pointer is still inside the viewport is only
            // a gap between materialized posts or the floating toolbar itself.
            // Keep both the toolbar and row highlight attached to the previous
            // semantic post.
            return;
        }

        setHoverHighlightOverride(row);

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

PostListWidget::~PostListWidget()
{
    // LongListWidget destroys materialized rows after our derived destructor.
    // Unbind their borrowed toolbar pointers while they are still alive.
    setToolbarTarget(nullptr);
    if (toolbarAnimation_) {
        toolbarAnimation_->stop();
    }
    delete toolbar_;
    toolbar_ = nullptr;
}

void PostListWidget::changeEvent(QEvent* event)
{
    LongListWidget::changeEvent(event);
    if (!event) {
        return;
    }
    const auto type = event->type();
    if (type == QEvent::PaletteChange
        || type == QEvent::ApplicationPaletteChange
        || type == QEvent::StyleChange) {
        refreshToolbarIcons();
        if (toolbarPost_) {
            updatePostHoverActionsPalette(*toolbarPost_);
        }
    }
}

void PostListWidget::ensureHoverActions()
{
    if (toolbar_) {
        return;
    }

    toolbar_ = new QFrame(viewport());
    toolbar_->setObjectName(QStringLiteral("postHoverActions"));
    toolbar_->setProperty("matterleastSharedPostHoverActions", true);
    toolbar_->setFrameShape(QFrame::NoFrame);
    toolbar_->setAutoFillBackground(false);
    toolbar_->hide();
    toolbar_->installEventFilter(this);

    auto* toolbarLayout = new QHBoxLayout(toolbar_);
    toolbarLayout->setContentsMargins(3, 2, 3, 2);
    toolbarLayout->setSpacing(0);

    quickBarSlot_ = new QWidget(toolbar_);
    quickBarSlot_->setObjectName(QStringLiteral("reactionQuickBarSlot"));
    quickBarSlot_->setMinimumWidth(0);
    quickBarSlot_->setMaximumWidth(0);
    quickBarSlot_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    auto* quickLayout = new QHBoxLayout(quickBarSlot_);
    quickLayout->setContentsMargins(0, 0, 0, 0);
    quickLayout->setSpacing(0);
    toolbarLayout->addWidget(quickBarSlot_);

    auto* actionsContainer = new QWidget(toolbar_);
    auto* actionsLayout = new QHBoxLayout(actionsContainer);
    actionsLayout->setContentsMargins(0, 0, 0, 0);
    actionsLayout->setSpacing(1);
    toolbarLayout->addWidget(actionsContainer);

    const auto makeButton = [this, actionsContainer, actionsLayout](
        const QString& iconPath, const QString& tooltip) {
        auto* button = new QPushButton(actionsContainer);
        button->setFlat(true);
        button->setFixedSize(28, 28);
        button->setCursor(Qt::PointingHandCursor);
        button->setIcon(IconUtils::symbolicIcon(iconPath));
        button->setToolTip(tooltip);
        button->setAccessibleName(tooltip);
        button->installEventFilter(this);
        actionsLayout->addWidget(button);
        return button;
    };

    reactionButton_ = makeButton(QStringLiteral(":/icons/emoji"), tr("Add reaction"));
    threadButton_ = makeButton(QStringLiteral(":/icons/message-balloon"), tr("Open thread"));
    saveButton_ = makeButton(QStringLiteral(":/icons/bookmark"), tr("Save message"));
    moreButton_ = makeButton(QStringLiteral(":/icons/more-horizontal"), tr("More actions"));
    _toolbarIconTint = QGuiApplication::palette().color(QPalette::WindowText);

    connect(reactionButton_, &QPushButton::clicked, this, [this] {
        QPointer<PostWidget> target = toolbarPost_;
        if (!target || target->post.isDeleted) {
            return;
        }
        showEmojiDialog([target](Emoji emoji) {
            if (target && !target->post.isDeleted) {
                target->getBackend().addPostReaction(target->post.id, emoji.name);
            }
        });
    });
    connect(threadButton_, &QPushButton::clicked, this, [this] {
        if (toolbarPost_ && !toolbarPost_->post.isDeleted) {
            toolbarPost_->openThreadWindow();
        }
    });
    connect(saveButton_, &QPushButton::clicked, this, [this] {
        if (!toolbarPost_ || toolbarPost_->post.isDeleted) {
            return;
        }
        PostWidget& post = *toolbarPost_;
        const BackendUserPreferences preference {
            QStringLiteral("flagged_post"), post.post.id, QStringLiteral("true")};
        if (post.getBackend().isPostFlagged(post.post.id)) {
            post.getBackend().deleteUserPreferences(preference);
        } else {
            post.getBackend().updateUserPreferences(preference);
        }
    });
    connect(moreButton_, &QPushButton::clicked, this, [this] {
        if (toolbarPost_ && !toolbarPost_->post.isDeleted) {
            toolbarPost_->showPostContextMenu(moreButton_->mapToGlobal(
                QPoint(0, moreButton_->height())));
        }
    });

    toolbarOpacity_ = new QGraphicsOpacityEffect(toolbar_);
    toolbarOpacity_->setOpacity(0.0);
    toolbar_->setGraphicsEffect(toolbarOpacity_);
    toolbarAnimation_ = new QPropertyAnimation(toolbarOpacity_, "opacity", this);
    toolbarAnimation_->setDuration(120);
    connect(toolbarAnimation_, &QPropertyAnimation::finished, this, [this] {
        if (toolbar_ && !toolbarWanted_) {
            toolbar_->hide();
        }
    });

    toolbarHideTimer_ = new QTimer(this);
    toolbarHideTimer_->setSingleShot(true);
    toolbarHideTimer_->setInterval(160);
    connect(toolbarHideTimer_, &QTimer::timeout, this, [this] {
        // underMouse() can remain stale when a Leave is lost. Recheck the
        // real pointer position against the current toolbar geometry instead.
        const bool insideToolbar = toolbar_ && toolbar_->isVisible()
            && toolbar_->rect().contains(toolbar_->mapFromGlobal(QCursor::pos()));
        // A toolbar Leave does not end the post hover session. Moving downward
        // from the reaction button into the same post may not produce a new
        // row Enter, so keeping the panel depends on its actual row geometry.
        if (!insideToolbar && !cursorOverActivePost()) {
            animateToolbar(false);
        }
    });
}

void PostListWidget::refreshToolbarIcons()
{
    if (!toolbar_) {
        return;
    }
    const QColor tint = QGuiApplication::palette().color(QPalette::WindowText);
    if (_toolbarIconTint == tint) {
        return;
    }
    _toolbarIconTint = tint;
    reactionButton_->setIcon(IconUtils::symbolicIcon(QStringLiteral(":/icons/emoji")));
    threadButton_->setIcon(IconUtils::symbolicIcon(QStringLiteral(":/icons/message-balloon")));
    saveButton_->setIcon(IconUtils::symbolicIcon(QStringLiteral(":/icons/bookmark")));
    moreButton_->setIcon(IconUtils::symbolicIcon(QStringLiteral(":/icons/more-horizontal")));
}

void PostListWidget::setToolbarTarget(PostWidget* post)
{
    if (post && toolbarPost_ == post) {
        return;
    }

    if (toolbar_) {
        // Hiding the old surface also collapses its ranked reaction expansion
        // before the same physical controls are assigned to another post.
        toolbar_->hide();
    }
    disconnect(toolbarPostDestroyed_);
    disconnect(toolbarFlaggedChanged_);

    if (auto* old = toolbarPost_.data()) {
        old->hoverActions_ = nullptr;
        old->reactionQuickBarSlot_ = nullptr;
        old->reactionAffordance_ = nullptr;
        old->threadAffordance_ = nullptr;
        old->saveAffordance_ = nullptr;
        old->moreAffordance_ = nullptr;
    }
    toolbarPost_ = post;
    if (!toolbar_) {
        return;
    }
    reactionButton_->setProperty("matterleastPostOwner",
        QVariant::fromValue(static_cast<QObject*>(post)));
    if (!post) {
        return;
    }

    post->hoverActions_ = toolbar_;
    post->reactionQuickBarSlot_ = quickBarSlot_;
    post->reactionAffordance_ = reactionButton_;
    post->threadAffordance_ = threadButton_;
    post->saveAffordance_ = saveButton_;
    post->moreAffordance_ = moreButton_;
    updateToolbarActions();
    updatePostHoverActionsPalette(*post);

    toolbarPostDestroyed_ = connect(post, &QObject::destroyed, this, [this] {
        // A virtualized post can disappear while the pointer is over chrome.
        // Never retain an action target after the row has been destroyed.
        toolbarPost_.clear();
        reactionButton_->setProperty("matterleastPostOwner",
            QVariant::fromValue(static_cast<QObject*>(nullptr)));
        hideToolbar(true);
    });
    toolbarFlaggedChanged_ = connect(&post->getBackend(),
        &Backend::onFlaggedPostChanged, this,
        [this](const QString& postId, bool flagged) {
            if (toolbarPost_ && toolbarPost_->post.id == postId) {
                saveButton_->setToolTip(
                    flagged ? tr("Remove from saved") : tr("Save message"));
            }
        });
}

void PostListWidget::updateToolbarActions()
{
    if (!toolbarPost_) {
        return;
    }
    PostWidget& post = *toolbarPost_;
    threadButton_->setVisible(post.parentChatArea
        && !post.parentChatArea->isThread && post.post.root_id.isEmpty());
    const QString saveText = post.getBackend().isPostFlagged(post.post.id)
        ? tr("Remove from saved") : tr("Save message");
    saveButton_->setToolTip(saveText);
    saveButton_->setAccessibleName(saveText);
}

void PostListWidget::animateToolbar(bool visible, bool immediate)
{
    toolbarWanted_ = visible && toolbarPost_ && !toolbarPost_->post.isDeleted;
    if (!toolbar_ || !toolbarOpacity_ || !toolbarAnimation_) {
        return;
    }
    toolbarAnimation_->stop();
    if (toolbarWanted_) {
        positionPostHoverActions(*toolbarPost_);
        toolbar_->show();
        toolbar_->raise();
    }
    if (immediate) {
        toolbarOpacity_->setOpacity(toolbarWanted_ ? 1.0 : 0.0);
        if (!toolbarWanted_) {
            toolbar_->hide();
        }
        return;
    }
    toolbarAnimation_->setStartValue(toolbarOpacity_->opacity());
    toolbarAnimation_->setEndValue(toolbarWanted_ ? 1.0 : 0.0);
    toolbarAnimation_->start();
}

bool PostListWidget::cursorOverActivePost() const
{
    const auto* post = toolbarPost_.data();
    if (!post || !post->isVisible() || !viewport()->isVisible()) {
        return false;
    }
    const QPoint global = QCursor::pos();
    return viewport()->rect().contains(viewport()->mapFromGlobal(global))
        && post->rect().contains(post->mapFromGlobal(global));
}

void PostListWidget::hideToolbar(bool immediate)
{
    if (toolbarHideTimer_) {
        toolbarHideTimer_->stop();
    }
    if (!immediate && toolbarHideTimer_) {
        toolbarHideTimer_->start();
        return;
    }
    animateToolbar(false, true);
}

void PostListWidget::showPostHoverActions(PostWidget& post, bool visible,
                                          bool immediate)
{
    if (!visible) {
        if (toolbarPost_ == &post) {
            hideToolbar(immediate);
        }
        return;
    }

    ensureHoverActions();
    if (toolbarHideTimer_) {
        toolbarHideTimer_->stop();
    }
    if (toolbarPost_ != &post) {
        setToolbarTarget(&post);
        immediate = true;
    }
    updateToolbarActions();
    animateToolbar(true, immediate);
}

void PostListWidget::positionPostHoverActions(const PostWidget& post)
{
    if (!toolbar_ || toolbarPost_ != &post) {
        return;
    }

    toolbar_->adjustSize();
    const int contentTop = post.dayBoundaryWidget_
        && !post.dayBoundaryWidget_->isHidden()
        ? post.dayBoundaryWidget_->sizeHint().height() : 0;
    const QPoint origin = post.mapTo(viewport(), QPoint(0, 0));
    const int maxX = std::max(0, viewport()->width() - toolbar_->width() - 4);
    const int maxY = std::max(0, viewport()->height() - toolbar_->height());
    toolbar_->move(
        std::max(0, std::min(origin.x() + post.width() - toolbar_->width() - 8, maxX)),
        std::max(0, std::min(origin.y() + contentTop - toolbar_->height() / 2, maxY)));
}

void PostListWidget::updatePostHoverActionsPalette(const PostWidget& post)
{
    if (!toolbar_ || toolbarPost_ != &post) {
        return;
    }
    const QColor background = hoverHighlightSurfaceColor();
    const QColor outline = post.palette().color(QPalette::Mid);

    QPalette barPalette = toolbar_->palette();
    barPalette.setColor(QPalette::Window, background);
    barPalette.setColor(QPalette::Button, background);
    toolbar_->setPalette(barPalette);

    const auto rgba = [](const QColor& c) {
        return QStringLiteral("rgba(%1,%2,%3,%4)")
            .arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
    };
    toolbar_->setStyleSheet(QStringLiteral(
        "QFrame#postHoverActions { background-color: %1;"
        " border: 1px solid %2; border-radius: 5px; }")
        .arg(rgba(background), rgba(outline)));
}

void PostListWidget::clearActiveHover(bool immediate)
{
    setHoverHighlightOverride(nullptr);
    if (auto* active = qobject_cast<PostWidget*>(activeHoverPost_.data())) {
        active->setHovered(false, immediate);
    }
    activeHoverPost_.clear();
    hoverSessionHasPost_ = false;
    // A missed row Leave may already have cleared the semantic hover owner.
    // Clearing the viewport/chrome hover must still dismiss its last toolbar.
    hideToolbar(immediate);
}

bool PostListWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (event && event->type() == QEvent::Enter
        && OverlayScrollBarManager::isEdgeNavigationExclusion(watched)) {
        // The guards around scroll-to-edge controls are direct scroll-area
        // children. End the semantic hover immediately, preventing the one
        // shared panel from capturing a navigation click.
        clearActiveHover(true);
    }

    if (event && toolbar_) {
        const auto type = event->type();
        if (watched == toolbar_ && type == QEvent::Enter && toolbarHideTimer_) {
            toolbarHideTimer_->stop();
        } else if (watched == toolbar_ && type == QEvent::Leave) {
            hideToolbar();
        } else if (watched == toolbar_ && type == QEvent::Hide
                   && toolbarHideTimer_) {
            toolbarHideTimer_->stop();
        } else if (type == QEvent::Enter
                   && (watched == reactionButton_ || watched == threadButton_
                       || watched == saveButton_ || watched == moreButton_)
                   && toolbarHideTimer_) {
            toolbarHideTimer_->stop();
        }
    }

    if (watched == viewport() && event) {
        if (event->type() == QEvent::Hide) {
            clearActiveHover(true);
            hideToolbar(true);
        } else if (event->type() == QEvent::Enter) {
            // Enter on the viewport may be a return from the floating toolbar
            // into the same post. There is no guaranteed subsequent row Enter
            // to restore hover, so don't clear a still-physically-hovered post.
            if (!cursorOverActivePost()) {
                clearActiveHover(true);
            }
        } else if (event->type() == QEvent::Leave) {
            clearActiveHover();
        } else if (event->type() == QEvent::Wheel) {
            clearActiveHover(true);
            hideToolbar(true);
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
