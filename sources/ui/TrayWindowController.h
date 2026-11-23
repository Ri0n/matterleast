#pragma once

#include <QByteArray>
#include <QEvent>
#include <QObject>
#include <QPointer>
#include <QWidget>

namespace Mattermost {

// The current tray target is weak: login dialogs can delete themselves.
// Preserve the top-level geometry across hide/show, including window-manager
// close-to-tray. Wayland compositors can assign a new (tiny) size to a
// withdrawn window when it is mapped again.
class TrayWindowController : public QObject
{
public:
    void setWindow(QWidget* window)
    {
        if (window_ == window)
            return;
        if (window_)
            window_->removeEventFilter(this);
        window_ = window;
        hiddenGeometry_.clear();
        if (window_)
            window_->installEventFilter(this);
    }

    QWidget* window() const { return window_.data(); }

    bool showWindow()
    {
        QWidget* target = window_.data();
        if (!target)
            return false;

        if (!target->isVisible() && !hiddenGeometry_.isEmpty())
            target->restoreGeometry(hiddenGeometry_);
        if (target->isMinimized())
            target->showNormal();
        else if (!target->isVisible())
            target->show();

        target->raise();
        target->activateWindow();
        return true;
    }

    bool toggleWindow()
    {
        QWidget* target = window_.data();
        if (!target)
            return false;

        if (target->isVisible() && !target->isMinimized())
            target->hide();
        else
            showWindow();
        return true;
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object == window_.data() && event->type() == QEvent::Hide) {
            // Capture the last real geometry before the window is remapped.
            // Do not update it during show/resize or when switching accounts.
            hiddenGeometry_ = window_->saveGeometry();
        }
        return QObject::eventFilter(object, event);
    }

private:
    QPointer<QWidget> window_;
    QByteArray hiddenGeometry_;
};

} // namespace Mattermost
