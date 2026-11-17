#pragma once

#include <QPointer>
#include <QWidget>

namespace Mattermost {

// QDialog may delete itself when closed (Qt::WA_DeleteOnClose). A raw pointer
// retained by tray activation would then call QWidget methods on freed memory.
// Keep the current target weak and let the caller recreate a login window
// when there is no live window to show.
class TrayWindowController
{
public:
    void setWindow(QWidget* window) { window_ = window; }
    QWidget* window() const { return window_.data(); }

    bool showWindow()
    {
        QWidget* target = window_.data();
        if (!target) {
            return false;
        }
        if (target->isMinimized()) {
            target->showNormal();
        } else if (!target->isVisible()) {
            target->show();
        }
        target->raise();
        target->activateWindow();
        return true;
    }

    bool toggleWindow()
    {
        QWidget* target = window_.data();
        if (!target) {
            return false;
        }
        if (target->isVisible()) {
            target->hide();
        } else {
            target->show();
        }
        return true;
    }

private:
    QPointer<QWidget> window_;
};

} // namespace Mattermost
