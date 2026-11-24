#include "mainwindow.h"

#include <QActionGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDateTimeEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui_mainwindow.h"
#include "backend/Backend.h"
#include "backend/CustomStatusValidation.h"
#include "ui/CustomStatusEditorDialog.h"
#include "backend/types/BackendUser.h"
#include "info-dialogs/UserProfileDialog.h"
#include "notifications/NotificationManager.h"

namespace Mattermost {

void MainWindow::reportStatusResult(bool success, const QString& reason)
{
    // A transport restart deliberately cancels old-route replies. The
    // server may already have processed such a mutation, so a canceled
    // response is not a user-facing status error.
    if (success || reason.isEmpty()) {
        return;
    }
    // Use the same native notification path as other application messages.
    // An empty activation target avoids raising/activating the main window.
    notificationManager->show(tr("Status update failed"), reason,
                              NotificationTarget {});
}

void MainWindow::on_usericon_label_clicked()
{
    // A late network recovery or session reset can temporarily leave the
    // profile widget alive without a logged-in user. getLoginUser()
    // dereferences that pointer, so inspect it before opening the menu.
    if (!backendOrNull()) return;
    const BackendUser* userPtr = backendInstance().getStorage().loginUser;
    if (!userPtr || userPtr->id.isEmpty()) {
        return;
    }
    const BackendUser& user = *userPtr;

    QMenu menu(this);
    auto* title = menu.addAction(tr("Availability"));
    title->setEnabled(false);

    QActionGroup availability(&menu);
    availability.setExclusive(true);
    const struct { const char* value; const char* title; } statuses[] = {
        {"online", QT_TR_NOOP("Online")},
        {"away", QT_TR_NOOP("Away")},
        {"dnd", QT_TR_NOOP("Do not disturb")},
        {"offline", QT_TR_NOOP("Offline")},
    };
    for (const auto& option : statuses) {
        const QString value = QString::fromLatin1(option.value);
        QAction* item = menu.addAction(tr(option.title));
        item->setCheckable(true);
        item->setChecked(user.status == value);
        availability.addAction(item);
        connect(item, &QAction::triggered, this, [this, value] {
            QPointer<MainWindow> guard(this);
            backendInstance().setOwnAvailability(value, [guard](bool ok, const QString& error) {
                if (guard) guard->reportStatusResult(ok, error);
            });
        });
    }

    menu.addSeparator();
    const QString currentCustom = user.props.customStatus.text;
    const QString currentEmoji = user.props.customStatus.emoji;
    const bool hasCustomStatus = hasCustomStatusContent(currentEmoji, currentCustom);
    if (hasCustomStatus) {
        const QString summary = currentCustom.isEmpty()
            ? tr(":%1:").arg(currentEmoji) : currentCustom;
        QAction* current = menu.addAction(tr("Custom: %1").arg(summary));
        current->setEnabled(false);
    }

    QAction* setCustom = menu.addAction(tr("Set a custom status…"));
    connect(setCustom, &QAction::triggered, this, [this] {
        CustomStatusEditorDialog editor(backendInstance(),
                                        backendInstance().getLoginUser().props.customStatus, this);
        QPointer<CustomStatusEditorDialog> editorGuard(&editor);
        QPointer<MainWindow> mainWindowGuard(this);
        editor.setRemoveRecentCallback(
            [this, mainWindowGuard](const BackendUserCustomStatus& status,
                                    std::function<void(bool)> done) {
                backendInstance().removeRecentCustomStatus(status,
                    [mainWindowGuard, done](bool ok, const QString& error) {
                        if (mainWindowGuard)
                            mainWindowGuard->reportStatusResult(ok, error);
                        done(ok);
                    });
            });
        backendInstance().retrieveRecentCustomStatuses(
            [editorGuard](QVector<BackendUserCustomStatus> statuses) {
                if (editorGuard)
                    editorGuard->setRecentStatuses(statuses);
            });

        if (editor.exec() != QDialog::Accepted)
            return;
        const QString emoji = editor.statusEmoji();
        const QString message = editor.statusText();
        auto finished = [mainWindowGuard](bool ok, const QString& error) {
            if (mainWindowGuard)
                mainWindowGuard->reportStatusResult(ok, error);
        };
        if (!hasCustomStatusContent(emoji, message)) {
            backendInstance().clearOwnCustomStatus(finished);
        } else {
            backendInstance().setOwnCustomStatus(emoji, message, editor.statusDuration(),
                                       editor.statusExpiresAt(), finished);
        }
    });

    QAction* clear = menu.addAction(tr("Clear custom status"));
    clear->setEnabled(hasCustomStatus);
    connect(clear, &QAction::triggered, this, [this] {
        QPointer<MainWindow> guard(this);
        backendInstance().clearOwnCustomStatus([guard](bool ok, const QString& error) {
            if (guard) guard->reportStatusResult(ok, error);
        });
    });

    menu.addSeparator();
    QAction* profile = menu.addAction(tr("View profile…"));
    connect(profile, &QAction::triggered, this, [this] {
        UserProfileDialog::showTransient(backendInstance(), backendInstance().getLoginUser(), this);
    });

    menu.exec(ui->usericon_label->mapToGlobal(QPoint(0, ui->usericon_label->height())));
}

} // namespace Mattermost
