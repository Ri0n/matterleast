/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QSplitter>

namespace Mattermost {

/**
 * A one-pixel splitter whose forgiving mouse target overlaps only the pane
 * after the divider. The pane before the divider is never covered by the
 * splitter hit area, so edge controls such as overlay scrollbars keep their
 * full input region.
 */
class ThinSplitter final : public QSplitter
{
public:
    static constexpr int VisibleHandleExtent = 1;
    static constexpr int TrailingHitPadding = 6;

    explicit ThinSplitter(Qt::Orientation orientation, QWidget* parent = nullptr);

protected:
    QSplitterHandle* createHandle() override;
};

} // namespace Mattermost
