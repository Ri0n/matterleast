/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "SplitterHandleManager.h"

#include <QApplication>
#include <QEvent>
#include <QPainter>
#include <QPalette>
#include <QSplitter>

#include "SplitterHandleStyle.h"

namespace Mattermost {
namespace {

constexpr char InstalledProperty[] = "mattermostSplitterHandleManagerInstalled";

} // namespace

void SplitterHandleManager::install(QApplication& application)
{
    if (application.property(InstalledProperty).toBool()) {
        return;
    }
    application.setProperty(InstalledProperty, true);
    new SplitterHandleManager(application);
}

SplitterHandleManager::SplitterHandleManager(QApplication& application)
    : QObject(&application)
{
    application.installEventFilter(this);
}

bool SplitterHandleManager::eventFilter(QObject* watched, QEvent* event)
{
    auto* handle = qobject_cast<QSplitterHandle*>(watched);
    if (!handle || !event || event->type() != QEvent::Paint) {
        return QObject::eventFilter(watched, event);
    }

    QPainter painter(handle);

    // Keep the 4 px mouse target, but make only one logical pixel visible.
    // The application palette is the source of the actual dark content surface;
    // sampling a child widget here is unreliable because its (0,0) can itself
    // contain a frame/border.
    const QColor background = qApp->palette().color(QPalette::Window);
    painter.fillRect(handle->rect(), background);

    const QColor divider = handle->palette().color(QPalette::Mid);
    const QRect dividerRect = handle->orientation() == Qt::Horizontal
        ? QRect(handle->rect().left(), handle->rect().top(),
                SplitterVisibleDividerExtent, handle->rect().height())
        : QRect(handle->rect().left(), handle->rect().top(),
                handle->rect().width(), SplitterVisibleDividerExtent);
    painter.fillRect(dividerRect, divider);
    return true;
}

} // namespace Mattermost
