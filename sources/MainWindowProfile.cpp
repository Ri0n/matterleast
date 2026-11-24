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
#include "ui/EmojiPresentation.h"
#include "ui/PresenceAvatarLabel.h"

namespace Mattermost {

void MainWindow::setProfileBackend(Backend* source)
{
    // Discard only subscriptions; header widgets and their layout survive.
    // This context is deliberately independent of the navigation/session
    // owner, so choosing a server need not change the current conversation.
    delete _profileSubscriptions;
    _profileSubscriptions = nullptr;
    _profileBackend = source;
    auto* avatar = ui->usericon_label;
    avatar->setConnectionIndicatorState(PresenceAvatarLabel::ConnectionIndicatorState::None);
    if (!source || !source->getStorage().loginUser) {
        avatar->setPixmap(QPixmap());
        avatar->setStatus(QString());
        avatar->setToolTip(QString());
        avatar->setEnabled(false);
        ui->usernameLabel->clear();
        ui->statusLabel->setFont(_defaultProfileStatusFont);
        ui->statusLabel->setContentsMargins(0, 0, 0, 0);
        ui->statusLabel->clear();
        return;
    }
    avatar->setEnabled(true);
    _profileSubscriptions = new QObject(this);
    auto* context = _profileSubscriptions;
    auto* user = source->getStorage().loginUser;
    const auto refresh = [this] { refreshProfileHeader(); };
    connect(user, &BackendUser::onStatusChanged, context, refresh);
    connect(user, &BackendUser::onAvatarChanged, context, refresh);
    connect(source, &Backend::onOwnCustomStatusChanged, context, refresh);
    connect(&source->emojiRegistry(), &EmojiRegistry::customEmojiAdded, context, refresh);
    connect(source, &QObject::destroyed, context, [this] { setProfileBackend(nullptr); });
    const auto connectionChanged = [avatar](WebSocketConnector::ConnectionState state) {
        using Indicator = PresenceAvatarLabel::ConnectionIndicatorState;
        Indicator value = Indicator::None;
        if (state == WebSocketConnector::ConnectionState::Connecting)
            value = Indicator::Connecting;
        else if (state == WebSocketConnector::ConnectionState::WaitingForReconnect)
            value = Indicator::WaitingForReconnect;
        avatar->setConnectionIndicatorState(value);
    };
    connect(source, &Backend::onWebSocketConnectionStateChanged, context, connectionChanged);
    connectionChanged(source->webSocketConnectionState());
    refreshProfileHeader();
}

void MainWindow::refreshProfileHeader()
{
    Backend* source = _profileBackend.data();
    const BackendUser* user = source ? source->getStorage().loginUser : nullptr;
    if (!user) return;
    auto* avatar = ui->usericon_label;
    avatar->setStatus(user->status);
    avatar->setPixmap(user->avatar);
    ui->usernameLabel->setText(user->username);
    const QString displayName = user->getDisplayName().trimmed();
    const QString accountName = user->username.trimmed();
    QStringList identity;
    if (!displayName.isEmpty()) identity.push_back(displayName);
    if (!accountName.isEmpty()
        && displayName.compare(accountName, Qt::CaseInsensitive) != 0)
        identity.push_back(QStringLiteral("@") + accountName);
    avatar->setToolTip(identity.join(QLatin1Char('\n')));

    const auto& custom = user->props.customStatus;
    const bool emojiOnly = isEmojiOnlyCustomStatus(custom.emoji, custom.text);
    ui->statusLabel->setFont(emojiOnly
        ? EmojiPresentation::fontForMode(_defaultProfileStatusFont,
                                        EmojiPresentation::Mode::StatusJumbo)
        : _defaultProfileStatusFont);
    ui->statusLabel->setContentsMargins(custom.emoji.isEmpty() ? 0 : 8, 0, 0, 0);
    if (custom.emoji.isEmpty()) {
        ui->statusLabel->setText(custom.text.isEmpty() ? user->status : custom.text);
        return;
    }
    auto& registry = source->emojiRegistry();
    const auto presentation = registry.resolveByName(custom.emoji);
    QString icon = presentation ? presentation->unicodeString
                                : QStringLiteral(":%1:").arg(custom.emoji.toHtmlEscaped());
    if (icon.contains(QStringLiteral("<img"), Qt::CaseInsensitive)) {
        icon = EmojiPresentation::normalizeHtml(
            icon, _defaultProfileStatusFont,
            emojiOnly ? EmojiPresentation::Mode::StatusJumbo : EmojiPresentation::Mode::Reaction,
            &registry);
    } else {
        icon = icon.toHtmlEscaped();
    }
    const QString message = custom.text.isEmpty()
        ? QString() : QStringLiteral(" ") + custom.text.toHtmlEscaped();
    ui->statusLabel->setText(QStringLiteral("<span>%1%2</span>").arg(icon, message));
}

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
    const QPointer<Backend> source = _profileBackend;
    if (!source) return;
    const BackendUser* userPtr = source->getStorage().loginUser;
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
        connect(item, &QAction::triggered, this, [this, source, value] {
            if (!source) return;
            QPointer<MainWindow> guard(this);
            source->setOwnAvailability(value, [guard](bool ok, const QString& error) {
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
    connect(setCustom, &QAction::triggered, this, [this, source] {
        if (!source || !source->getStorage().loginUser) return;
        CustomStatusEditorDialog editor(*source,
                                        source->getLoginUser().props.customStatus, this);
        connect(source, &QObject::destroyed, &editor, &QDialog::reject);
        QPointer<CustomStatusEditorDialog> editorGuard(&editor);
        QPointer<MainWindow> mainWindowGuard(this);
        editor.setRemoveRecentCallback(
            [source, mainWindowGuard](const BackendUserCustomStatus& status,
                                    std::function<void(bool)> done) {
                if (!source) { done(false); return; }
                source->removeRecentCustomStatus(status,
                    [mainWindowGuard, done](bool ok, const QString& error) {
                        if (mainWindowGuard)
                            mainWindowGuard->reportStatusResult(ok, error);
                        done(ok);
                    });
            });
        source->retrieveRecentCustomStatuses(
            [editorGuard](QVector<BackendUserCustomStatus> statuses) {
                if (editorGuard)
                    editorGuard->setRecentStatuses(statuses);
            });

        if (editor.exec() != QDialog::Accepted || !source)
            return;
        const QString emoji = editor.statusEmoji();
        const QString message = editor.statusText();
        auto finished = [mainWindowGuard](bool ok, const QString& error) {
            if (mainWindowGuard)
                mainWindowGuard->reportStatusResult(ok, error);
        };
        if (!hasCustomStatusContent(emoji, message)) {
            source->clearOwnCustomStatus(finished);
        } else {
            source->setOwnCustomStatus(emoji, message, editor.statusDuration(),
                                       editor.statusExpiresAt(), finished);
        }
    });

    QAction* clear = menu.addAction(tr("Clear custom status"));
    clear->setEnabled(hasCustomStatus);
    connect(clear, &QAction::triggered, this, [this, source] {
        if (!source) return;
        QPointer<MainWindow> guard(this);
        source->clearOwnCustomStatus([guard](bool ok, const QString& error) {
            if (guard) guard->reportStatusResult(ok, error);
        });
    });

    menu.addSeparator();
    QAction* profile = menu.addAction(tr("View profile…"));
    connect(profile, &QAction::triggered, this, [this, source] {
        if (source && source->getStorage().loginUser)
            UserProfileDialog::showTransient(*source, source->getLoginUser(), this);
    });

    menu.exec(ui->usericon_label->mapToGlobal(QPoint(0, ui->usericon_label->height())));
}

} // namespace Mattermost
