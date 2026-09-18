/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <QObject>

class QApplication;
class QEvent;

namespace Mattermost {

/** Paint splitter handles as a thin divider while preserving their full hit area. */
class SplitterHandleManager final : public QObject
{
public:
    static void install(QApplication& application);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    explicit SplitterHandleManager(QApplication& application);
};

} // namespace Mattermost
