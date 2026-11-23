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

#pragma once

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QLabel;
#include "backend/types/BackendLoginData.h"

namespace Ui {
class LoginDialog;
}

namespace Mattermost {

class Backend;

class LoginDialog: public QDialog {
	Q_OBJECT
public:
    // This dialog is a credential editor only. No legacy settings, keyring
    // restore or migration code belongs in Widgets.
    LoginDialog(QWidget* parent, Backend& backend,
                const QString& server = {},
                const QString& login = {},
                bool lockIdentity = false);
    // New server: credentials only. Accept closes the form; AccountManager
    // performs the asynchronous connection in the visible Servers tree.
    LoginDialog(QWidget* parent, const QString& server,
                const QString& login);
    BackendLoginData submittedCredentials() const { return _submittedCredentials; }
    void configureExistingAccount(const QString& name, const QString& userId,
                                  bool connected);
    QString displayName() const;
    bool submittedNewCredentials() const { return _credentialsSubmitted; }
    QString authenticatedLoginName() const { return _authenticatedLoginName; }
	~LoginDialog ();
private slots:
	void on_login_pushButton_clicked();

	void onNetworkError (uint32_t errorNumber, const QString& errorText);
	void onHttpError (uint32_t errorNumber, const QString& errorText);

private:
    LoginDialog(QWidget* parent, Backend* backend,
                const QString& server, const QString& login, bool lockIdentity);
	void setError (const QString& errorStr);
	void loginToServer (const BackendLoginData& loginData);

	Backend*			backend = nullptr;
    BackendLoginData _submittedCredentials;
	Ui::LoginDialog*	ui;
    QCheckBox* _tokenMode = nullptr;
    QLineEdit* _displayName = nullptr;
    QLabel* _serverUserInfo = nullptr;
    bool _editingExisting = false;
    bool _editingConnected = false;
    bool _credentialsSubmitted = false;
    QString _authenticatedLoginName;
    bool _attemptInProgress = false;
    quint64 _loginAttemptGeneration = 0;
};

} /* namespace Mattermost */
