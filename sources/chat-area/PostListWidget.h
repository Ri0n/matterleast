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

#pragma once

#include <QColor>
#include <QPointer>
#include <QMetaObject>

#include "widgets/LongListWidget.h"

class QFrame;
class QGraphicsOpacityEffect;
class QPropertyAnimation;
class QPushButton;
class QTimer;
class QEvent;

namespace Mattermost {
class PostWidget;

/**
 * Shared presentation policy for virtualized lists whose rows contain posts.
 *
 * LongListWidget deliberately owns only generic virtualization and scrolling.
 * PostListWidget adds the visual/list tuning common to chat timelines and
 * Saved/Search/Pinned post collections so those views cannot drift apart.
 */
class PostListWidget : public LongListWidget
{
public:
    explicit PostListWidget(QWidget* parent = nullptr);
    ~PostListWidget() override;

    // One floating action frame is owned by each list, never by its rows.
    void showPostHoverActions(PostWidget& post, bool visible, bool immediate);
    void positionPostHoverActions(const PostWidget& post);
    void updatePostHoverActionsPalette(const PostWidget& post);

protected:
    /** All post-based LongLists use the semantic post ID as their stable row key. */
    QString itemIdentity(const QWidget* widget) const override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void clearActiveHover(bool immediate = false);
    void ensureHoverActions();
    void setToolbarTarget(PostWidget* post);
    void animateToolbar(bool visible, bool immediate = false);
    void updateToolbarActions();
    void refreshToolbarIcons();
    void hideToolbar(bool immediate = false);
    bool cursorOverActivePost() const;

    QPointer<PostWidget> toolbarPost_;
    QFrame* toolbar_ = nullptr;
    QWidget* quickBarSlot_ = nullptr;
    QPushButton* reactionButton_ = nullptr;
    QPushButton* threadButton_ = nullptr;
    QPushButton* saveButton_ = nullptr;
    QPushButton* moreButton_ = nullptr;
    QGraphicsOpacityEffect* toolbarOpacity_ = nullptr;
    QPropertyAnimation* toolbarAnimation_ = nullptr;
    QTimer* toolbarHideTimer_ = nullptr;
    QMetaObject::Connection toolbarPostDestroyed_;
    QMetaObject::Connection toolbarFlaggedChanged_;
    bool toolbarWanted_ = false;
    QColor _toolbarIconTint;

    QPointer<QWidget> activeHoverPost_;
    bool hoverSessionHasPost_ = false;
};

} // namespace Mattermost
