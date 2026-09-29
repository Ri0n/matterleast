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

#include <QPointer>

#include "widgets/LongListWidget.h"

namespace Mattermost {

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

protected:
    /** All post-based LongLists use the semantic post ID as their stable row key. */
    QString itemIdentity(const QWidget* widget) const override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QPointer<QWidget> activeHoverPost_;
    bool hoverSessionHasPost_ = false;
};

} // namespace Mattermost
