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

    // The handle remains 4 px wide for mouse interaction. Visually it belongs
    // to the content surface: fill the whole hit area with the adjacent chat
    // background and draw a single separator line on its leading edge.
    const QSplitter* splitter = qobject_cast<const QSplitter*>(handle->parentWidget());
    int handleIndex = -1;
    if (splitter) {
        for (int index = 1; index < splitter->count(); ++index) {
            if (splitter->handle(index) == handle) {
                handleIndex = index;
                break;
            }
        }
    }

    // Sample the actual rendered surface immediately to the right of the
    // divider. The chat surfaces can be styled independently of QPalette::Window,
    // so using the palette here produced a visibly different 4 px strip.
    QColor background = handle->palette().color(QPalette::Window);
    if (splitter && handleIndex > 0) {
        QWidget* content = splitter->widget(handleIndex);
        if (content) {
            const QPixmap sample = content->grab(QRect(0, 0, 1, 1));
            if (!sample.isNull()) {
                background = sample.toImage().pixelColor(0, 0);
            }
        }
    }
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
