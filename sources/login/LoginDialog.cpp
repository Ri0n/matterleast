/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

#include <iostream>
#include "LoginDialog.h"
#include "ui_LoginDialog.h"

#include "backend/Backend.h"
#include "backend/SessionTokenStore.h"
#include "options/MLOptions.h"
#include "log.h"
#include <QPointer>
#include <QTimer>
#include "ui/IconUtils.h"

namespace Mattermost {

LoginDialog::LoginDialog (QWidget *parent, Backend& backend)
:QDialog(parent)
,backend (backend)
,ui(new Ui::LoginDialog)
{
	setAttribute (Qt::WA_DeleteOnClose);
	BackendLoginData loginData;
	loginData.loadFromOptions ();

	ui->setupUi(this);

	connect (&backend, &Backend::onNetworkError, this, &LoginDialog::onNetworkError);
	connect (&backend, &Backend::onHttpError, this, &LoginDialog::onHttpError);

	if (loginData.domain.isEmpty()) {
		loginData.domain = "https://";
	}

	ui->domain_lineEdit->setText (loginData.domain);
	ui->username_lineEdit->setText (loginData.username);

	ui->icon->setPixmap (IconUtils::applicationIcon().pixmap (QSize (64, 64)));
	ui->loginInProgressLabel->hide();

    _autoLoginRetryTimer.setSingleShot(true);
    connect(&_autoLoginRetryTimer, &QTimer::timeout, this, [this] {
        if (_autoLoginEnabled && !_attemptInProgress) {
            loginToServer(_savedLoginData);
        }
    });

    // Keyring access can unlock the desktop wallet. Never block the UI or
    // attempt password login while an asynchronous read is pending.
    if (backend.autoLoginEnabled() && !loginData.domain.isEmpty()
        && !loginData.username.isEmpty()) {
        _autoLoginEnabled = true;
        QPointer<LoginDialog> guard(this);
        SessionTokenStore::read(
            this, loginData.domain, loginData.username,
            [this, guard, loginData](SessionTokenStore::ReadStatus status,
                                     const QString& secureToken) mutable {
                if (!guard || !_autoLoginEnabled || _attemptInProgress) return;
                if (status == SessionTokenStore::ReadStatus::Found) {
                    loginData.token = secureToken;
                    // A previous plaintext entry is obsolete once the same
                    // account has a successfully retrieved keyring secret.
                    MLOptions::instance()->setValue<QString>(
                        QStringLiteral("token"), QString());
                } else if (status == SessionTokenStore::ReadStatus::NotFound
                           && !loginData.token.isEmpty()) {
                    // One-time migration: authenticate with the old token.
                    // Its settings copy is cleared only when saving to the
                    // keyring succeeds after authentication.
                } else {
                    // Keyring unavailable is not "entry not found".
                    // Preserve legacy settings on temporary keyring errors.
                    _autoLoginEnabled = false;
                    return;
                }
                if (!loginData.areAllFieldsFilled()) {
                    _autoLoginEnabled = false;
                    return;
                }
                _savedLoginData = loginData;
                loginToServer(loginData);
            });
    }
}

LoginDialog::~LoginDialog()
{
    delete ui;
}

void LoginDialog::on_login_pushButton_clicked()
{
    _autoLoginEnabled = false;
    _autoLoginRetryTimer.stop();
    if (_attemptInProgress) return;
	BackendLoginData loginData;

	loginData.domain = ui->domain_lineEdit->text();
	loginData.username = ui->username_lineEdit->text();
	loginData.password = ui->password_lineEdit->text();
	loginData.token = "";

    loginData.saveToOptions();
    // Explicitly switching credentials invalidates a legacy settings token.
    // Otherwise a crash before the new keyring write could bind the old
    // plaintext session to the newly selected server/username.
    MLOptions::instance()->setValue<QString>(QStringLiteral("token"), QString());
    loginToServer(loginData);
}


void LoginDialog::loginToServer (const BackendLoginData& loginData)
{
    if (_attemptInProgress) return;
    _attemptInProgress = true;
    ui->domain_lineEdit->setEnabled(false);
    ui->username_lineEdit->setEnabled(false);
    ui->password_lineEdit->setEnabled(false);
    ui->login_pushButton->setEnabled(false);
    ui->error_label->clear();
    ui->loginInProgressLabel->show();

    LOG_DEBUG ("LoginDialog login");
    QPointer<LoginDialog> guard(this);
    backend.login(loginData, [this, guard, loginData](const QString& token) mutable {
        if (!guard) return;
        _attemptInProgress = false;
        if (!token.isEmpty()) {
            _autoLoginRetryTimer.stop();
            _autoLoginEnabled = false;
            // The backend outlives this login dialog. Save asynchronously
            // there, so closing the dialog cannot cancel the keyring job.
            // Never write a new token into plaintext application settings.
            SessionTokenStore::write(
                &backend, loginData.domain, loginData.username, token,
                [](bool success) {
                    if (success) {
                        MLOptions::instance()->setValue<QString>(
                            QStringLiteral("token"), QString());
                    }
                });
            accept();
            return;
        }

        if (_autoLoginEnabled && backend.lastLoginFailureTransient()) {
            // Auto-login is an offline-capable startup path: stay in the
            // login dialog until /users/me returned a valid user identity.
            // Never create a partially initialized MainWindow.
            ui->loginInProgressLabel->setText(
                tr("Server unavailable. Retrying saved session…"));
            _autoLoginRetryTimer.start(3000);
            return;
        }

        _autoLoginEnabled = false;
        _autoLoginRetryTimer.stop();
        setError(loginData.token.isEmpty()
            ? tr("Login failed. Check your credentials or network connection.")
            : tr("Saved session rejected. Please sign in again."));
        ui->loginInProgressLabel->hide();
    });
}

void LoginDialog::onNetworkError (uint32_t errorNumber, const QString& errorText)
{
    qCritical() << "Net error: " << errorNumber << ": " << errorText;
    if (_autoLoginEnabled) {
        // Keep the manual form disabled until the saved-session attempt
        // finishes; its callback owns the retry timer and error state.
        ui->error_label->setText(tr("Network error: %1").arg(errorText));
        return;
    }
    setError("Network Error: " + errorText);
    ui->loginInProgressLabel->hide();
}

void LoginDialog::onHttpError (uint32_t errorNumber, const QString& errorText)
{
	if (errorText.isEmpty() && !ui->error_label->text().isEmpty()) {
		return;
	} else {
		setError ("HTTP Error " + QString::number(errorNumber) + ": "  + errorText);
	}
	ui->loginInProgressLabel->hide();
}

void LoginDialog::setError (const QString& errorStr)
{
    ui->domain_lineEdit->setEnabled(true);
    ui->username_lineEdit->setEnabled(true);
    ui->password_lineEdit->setEnabled(true);
    ui->login_pushButton->setEnabled(true);

    ui->error_label->setText (errorStr);
}

} /* namespace Mattermost */

