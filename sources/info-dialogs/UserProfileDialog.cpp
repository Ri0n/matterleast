/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "UserProfileDialog.h"
#include "ui_UserProfileDialog.h"

#include <memory>

#include <QDialogButtonBox>
#include <QEvent>
#include <QLayoutItem>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendDirectChannelsTeam.h"
#include "backend/types/BackendUser.h"
#include "ui/UserStatusEmoji.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {
namespace {

constexpr int ProfileAvatarSize = 128;

QString getString(const QString& str)
{
    return str.isEmpty() ? QStringLiteral("N/A") : str;
}

} // namespace

UserProfileDialog::UserProfileDialog(const BackendUser& user, QWidget* parent)
    : UserProfileDialog(nullptr, user, parent)
{
}

UserProfileDialog::UserProfileDialog(Backend& backend, const BackendUser& user, QWidget* parent)
    : UserProfileDialog(&backend, user, parent)
{
}

UserProfileDialog::UserProfileDialog(Backend* backendInstance,
                                     const BackendUser& user,
                                     QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::UserProfileDialog)
    , backend(backendInstance)
    , userId(user.id)
{
    ui->setupUi(this);

    // Keep the primary profile action on the same baseline as Close. ActionRole
    // is ordered on the opposite side of the standard reject/close buttons by
    // the active platform style, instead of maintaining a second button row.
    ui->buttonBox->addButton(ui->messageButton, QDialogButtonBox::ActionRole);
    while (QLayoutItem* item = ui->actionsLayout->takeAt(0)) {
        delete item;
    }

    setWindowTitle(QStringLiteral("Profile for ") + user.getDisplayName()
                   + QStringLiteral(" - Mattermost"));

    ui->avatar->setAlignment(Qt::AlignCenter);

    if (backend && !user.id.isEmpty()) {
        // Backend-backed profile windows must share the same avatar state as
        // chat headers, post rows and member lists. The old private GET path
        // only painted this dialog and left BackendUser::avatar unchanged.
        if (BackendUser* storedUser = backend->getStorage().getUserById(user.id)) {
            connect(storedUser, &BackendUser::onAvatarChanged,
                    this, &UserProfileDialog::refreshSharedAvatar);
            refreshSharedAvatar();
            UserProfileService::instance(*backend).ensureAvatar(*storedUser);
        } else if (!user.avatar.isNull()) {
            ui->avatar->setPixmap(user.avatar.scaled(ProfileAvatarSize,
                                                      ProfileAvatarSize,
                                                      Qt::KeepAspectRatio,
                                                      Qt::SmoothTransformation));
        } else {
            ui->avatar->clear();
        }
    } else {
        // Keep the backend-less constructor functional for legacy callers. It
        // has no shared Storage/UserProfileService to publish the avatar into.
        if (!user.avatar.isNull()) {
            ui->avatar->setPixmap(user.avatar.scaled(ProfileAvatarSize,
                                                      ProfileAvatarSize,
                                                      Qt::KeepAspectRatio,
                                                      Qt::SmoothTransformation));
        } else {
            ui->avatar->clear();
        }

        if (!user.id.isEmpty()) {
            const QString pictureVersion = QString::number(
                static_cast<qulonglong>(user.last_picture_update));
            NetworkRequest request(
                QStringLiteral("users/") + user.id + QStringLiteral("/image?_=")
                    + pictureVersion,
                true);
            request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                                 QNetworkRequest::PreferCache);

            QPointer<UserProfileDialog> guard(this);
            avatarConnector.get(request, HttpResponseCallback(
                [guard](QByteArray avatarData) {
                    if (!guard) {
                        return;
                    }
                    QPixmap pixmap;
                    if (!pixmap.loadFromData(avatarData)) {
                        return;
                    }
                    guard->ui->avatar->setPixmap(
                        pixmap.scaled(ProfileAvatarSize,
                                      ProfileAvatarSize,
                                      Qt::KeepAspectRatio,
                                      Qt::SmoothTransformation));
                }));
        }
    }

    ui->fullnameValue->setText(user.first_name + QLatin1Char(' ') + user.last_name);
    ui->nicknameValue->setText(getString(user.nickname));
    ui->usernameValue->setText(user.username);
    ui->emailValue->setText(user.email);
    ui->positionValue->setText(getString(user.position));
    ui->statusValue->setText(user.status);
    ui->timezoneValue->setText(user.timezone.automaticTimezone);

    // Custom status is distinct from the availability state above.
    // Reuse the cached user.props payload; do not fetch it separately.
    refreshCustomStatus(user);
    if (backend && !userId.isEmpty()) {
        connect(&UserProfileService::instance(*backend),
                &UserProfileService::profileResolved, this,
                [this](const QString& resolvedId) {
            if (resolvedId == userId) refreshStoredCustomStatus();
        });
        connect(&backend->emojiRegistry(), &EmojiRegistry::customEmojiAdded,
                this, [this](const QString& emojiName) {
            if (!backend) return;
            const BackendUser* stored = backend->getStorage().getUserById(userId);
            if (stored && UserStatusEmoji::name(*stored) == emojiName) {
                refreshCustomStatus(*stored);
            }
        });
        refreshStoredCustomStatus();
    }

    ui->messageButton->setVisible(backend != nullptr && !user.id.isEmpty());
    if (backend) {
        connect(ui->messageButton, &QPushButton::clicked,
                this, &UserProfileDialog::startDirectMessage);
    }
}

UserProfileDialog::~UserProfileDialog()
{
    delete ui;
}

void UserProfileDialog::refreshStoredCustomStatus()
{
    if (!backend || userId.isEmpty()) return;
    BackendUser* stored = backend->getStorage().getUserById(userId);
    if (!stored) return;
    connect(stored, &BackendUser::onCustomStatusChanged,
            this, &UserProfileDialog::refreshStoredCustomStatus,
            Qt::UniqueConnection);
    refreshCustomStatus(*stored);
}

void UserProfileDialog::refreshCustomStatus(const BackendUser& user)
{
    const QString emojiName = UserStatusEmoji::name(user);
    const QString message = UserStatusEmoji::text(user);
    const bool hasStatus = !emojiName.isEmpty() || !message.isEmpty();
    if (!hasStatus) {
        ui->customStatusEmoji->clear();
        ui->customStatusText->clear();
        ui->customStatusRow->hide();
        return;
    }

    if (!emojiName.isEmpty()) {
        // Backend-less legacy dialogs can still render built-in emoji.
        static EmojiRegistry builtins;
        EmojiRegistry& registry = backend ? backend->emojiRegistry() : builtins;
        const QPixmap image = UserStatusEmoji::pixmap(
            registry, user, 20, ui->customStatusEmoji->font());
        if (!image.isNull()) {
            ui->customStatusEmoji->setPixmap(image);
        } else {
            // Keep emoji-only statuses visible even if the image is still
            // being resolved from the server.
            ui->customStatusEmoji->setText(
                QStringLiteral(":%1:").arg(emojiName));
        }
        ui->customStatusEmoji->show();
    } else {
        ui->customStatusEmoji->clear();
        ui->customStatusEmoji->hide();
    }
    ui->customStatusText->setText(message);
    ui->customStatusText->setVisible(!message.isEmpty());
    // Show the containing row last, after the emoji-only layout is complete.
    ui->customStatusRow->show();
}

void UserProfileDialog::refreshSharedAvatar()
{
    if (!backend || !ui || userId.isEmpty()) {
        return;
    }

    const BackendUser* user = backend->getStorage().getUserById(userId);
    if (!user || user->avatar.isNull()) {
        ui->avatar->clear();
        return;
    }

    ui->avatar->setPixmap(user->avatar.scaled(ProfileAvatarSize,
                                               ProfileAvatarSize,
                                               Qt::KeepAspectRatio,
                                               Qt::SmoothTransformation));
}

UserProfileDialog* UserProfileDialog::showTransient(Backend& backend,
                                                    const BackendUser& user,
                                                    QWidget* parent)
{
    return showTransient(new UserProfileDialog(backend, user, parent));
}

UserProfileDialog* UserProfileDialog::showTransient(const BackendUser& user,
                                                    QWidget* parent)
{
    return showTransient(new UserProfileDialog(user, parent));
}

UserProfileDialog* UserProfileDialog::showTransient(UserProfileDialog* dialog)
{
    if (!dialog) {
        return nullptr;
    }
    dialog->setWindowModality(Qt::NonModal);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
    return dialog;
}

bool UserProfileDialog::event(QEvent* event)
{
    if (event && event->type() == QEvent::WindowDeactivate
        && isVisible() && !isModal()) {
        // Closing from inside WindowDeactivate can destroy a delete-on-close
        // instance while Qt is still dispatching this event. Queue the close to
        // the next event-loop turn; visually it is still an immediate dismissal.
        QTimer::singleShot(0, this, &QWidget::close);
    }
    return QDialog::event(event);
}

void UserProfileDialog::startDirectMessage()
{
    if (!backend || userId.isEmpty()) {
        return;
    }

    BackendUser* user = backend->getStorage().getUserById(userId);
    if (!user) {
        return;
    }

    if (BackendChannel* channel = backend->getStorage().getDirectChannelByUserId(userId)) {
        AppNavigationService::instance(*backend).openChannel(channel->id);
        accept();
        return;
    }

    // New direct channels arrive asynchronously through the ordinary backend
    // channel path. Keep one backend-owned connection alive after this dialog
    // closes and navigate exactly once when the requested user's DM appears.
    Backend* backendInstance = backend;
    const QString requestedUserId = userId;
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(&backendInstance->getStorage().directChannels,
                          &BackendDirectChannelsTeam::onNewChannel,
                          backendInstance,
                          [backendInstance, requestedUserId, connection](BackendChannel& channel) {
        if (channel.type != BackendChannel::directChannel
            || channel.name != requestedUserId) {
            return;
        }
        QObject::disconnect(*connection);
        AppNavigationService::instance(*backendInstance).openChannel(channel.id);
    });

    ui->messageButton->setEnabled(false);
    ui->messageButton->setText(tr("Starting…"));
    backend->createDirectChannel(*user);
    accept();
}

} /* namespace Mattermost */
