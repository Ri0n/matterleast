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
#include <QCheckBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
#include "LoginDialog.h"
#include "ui_LoginDialog.h"

#include "backend/Backend.h"
#include "log.h"
#include <QPointer>
#include "ui/IconUtils.h"

namespace Mattermost {

LoginDialog::LoginDialog(QWidget* parent, Backend& backend,
                         const QString& server,
                         const QString& login,
                         bool lockIdentity)
    : LoginDialog(parent, &backend, server, login, lockIdentity)
{
}

LoginDialog::LoginDialog(QWidget* parent, const QString& server,
                         const QString& login)
    : LoginDialog(parent, nullptr, server, login, false)
{
}

LoginDialog::LoginDialog(QWidget* parent, Backend* backend,
                         const QString& server,
                         const QString& login,
                         bool lockIdentity)
    : QDialog(parent)
    , backend(backend)
    , ui(new Ui::LoginDialog)
{
    setAttribute(Qt::WA_DeleteOnClose);
    ui->setupUi(this);
    connect(ui->cancel_pushButton, &QPushButton::clicked,
            this, &QDialog::reject);
    ui->loginInProgressLabel->setText(tr("Signing in…"));

    if (backend) {
        connect(backend, &Backend::onNetworkError,
                this, &LoginDialog::onNetworkError);
        connect(backend, &Backend::onHttpError,
                this, &LoginDialog::onHttpError);
    }

    ui->domain_lineEdit->setText(server.isEmpty()
        ? QStringLiteral("https://") : server);
    ui->username_lineEdit->setText(login);
    // Add and Edit share the same credential form. The optional friendly
    // alias does not affect the immutable server/user authentication identity.
    auto* aliasLabel = new QLabel(tr("Display name"), this);
    _displayName = new QLineEdit(this);
    _displayName->setObjectName(QStringLiteral("serverDisplayName"));
    _displayName->setPlaceholderText(tr("Optional server name"));
    ui->gridLayout->addWidget(aliasLabel, 1, 0);
    ui->gridLayout->addWidget(_displayName, 1, 1);
    _serverUserInfo = new QLabel(this);
    _serverUserInfo->setObjectName(QStringLiteral("serverUserIdInfo"));
    _serverUserInfo->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _serverUserInfo->setWordWrap(true);
    _serverUserInfo->hide();
    ui->gridLayout->addWidget(_serverUserInfo, 5, 0, 1, 2);

    if (lockIdentity) {
        ui->domain_lineEdit->setReadOnly(true);
        ui->username_lineEdit->setReadOnly(true);
        ui->titleLabel->setText(tr("Sign in to this account"));
    }

    _tokenMode = new QCheckBox(tr("Use personal access token"), this);
    _tokenMode->setObjectName(QStringLiteral("personalAccessTokenCheck"));
    ui->verticalLayout->insertWidget(3, _tokenMode);
    connect(_tokenMode, &QCheckBox::toggled, this, [this](bool enabled) {
        ui->password_label->setText(enabled ? tr("Token") : tr("Password"));
        ui->password_lineEdit->setPlaceholderText(
            enabled ? tr("Personal access token") : tr("Password"));
        ui->password_lineEdit->clear();
    });

    ui->icon->setPixmap(IconUtils::applicationIcon().pixmap(QSize(48, 48)));
    ui->loginInProgressLabel->hide();
}

void LoginDialog::configureExistingAccount(const QString& name,
                                           const QString& userId,
                                           bool connected)
{
    _editingExisting = true;
    _editingConnected = connected;
    _displayName->setText(name);
    ui->domain_lineEdit->setReadOnly(true);
    ui->username_lineEdit->setReadOnly(true);
    ui->titleLabel->setText(tr("Edit server"));
    ui->login_pushButton->setText(tr("Save"));
    if (!userId.isEmpty()) {
        _serverUserInfo->setText(tr("Server user ID: %1").arg(userId));
        _serverUserInfo->show();
    }
    if (connected) {
        // Cannot adopt another Backend for the same authenticated identity
        // while the live connection and its Widgets are still in use.
        ui->password_lineEdit->setEnabled(false);
        _tokenMode->setEnabled(false);
        ui->password_lineEdit->setPlaceholderText(
            tr("Disconnect to change credentials"));
    }
}

QString LoginDialog::displayName() const
{
    return _displayName ? _displayName->text().trimmed() : QString();
}

LoginDialog::~LoginDialog()
{
    delete ui;
}

void LoginDialog::on_login_pushButton_clicked()
{
    if (_attemptInProgress)
        return;
    BackendLoginData credentials;
    credentials.domain = ui->domain_lineEdit->text().trimmed();
    credentials.username = ui->username_lineEdit->text().trimmed();
    if (_editingExisting && (_editingConnected
                             || ui->password_lineEdit->text().isEmpty())) {
        // Alias-only edit: no authentication, token mutation or Backend.
        _credentialsSubmitted = false;
        accept();
        return;
    }
    if (_tokenMode->isChecked())
        credentials.token = ui->password_lineEdit->text();
    else
        credentials.password = ui->password_lineEdit->text();
    const QUrl endpoint(credentials.domain);
    if (!endpoint.isValid() || endpoint.host().isEmpty()
        || (endpoint.scheme() != QLatin1String("https")
            && endpoint.scheme() != QLatin1String("http"))) {
        setError(tr("Enter a valid http:// or https:// server URL."));
        return;
    }
    if (credentials.username.isEmpty()
        || (credentials.password.isEmpty() && credentials.token.isEmpty())) {
        setError(tr("Enter your username and password or access token."));
        return;
    }
    if (!backend) {
        // New server setup: never perform a login while the modal form is
        // open. AccountManager now owns connection progress and failures.
        _submittedCredentials = credentials;
        _credentialsSubmitted = true;
        _authenticatedLoginName = credentials.username;
        accept();
        return;
    }
    loginToServer(credentials);
}

void LoginDialog::loginToServer(const BackendLoginData& credentials)
{
    if (_attemptInProgress)
        return;
    _attemptInProgress = true;
    const quint64 generation = ++_loginAttemptGeneration;
    ui->domain_lineEdit->setEnabled(false);
    ui->username_lineEdit->setEnabled(false);
    ui->password_lineEdit->setEnabled(false);
    ui->login_pushButton->setEnabled(false);
    _tokenMode->setEnabled(false);
    ui->error_label->clear();
    ui->loginInProgressLabel->show();

    QPointer<LoginDialog> guard(this);
    backend->login(credentials,
                  [this, guard, credentials, generation](const QString& token) {
        if (!guard || generation != _loginAttemptGeneration)
            return;
        _attemptInProgress = false;
        if (!token.isEmpty()) {
            _credentialsSubmitted = true;
            _authenticatedLoginName = credentials.username;
            // Authentication / keyring persistence belongs to AccountManager.
            // Versioned legacy fallback belongs to migrations/.
            accept();
        } else {
            setError(tr("Login failed. Check your credentials or network connection."));
            ui->loginInProgressLabel->hide();
        }
    });
}

void LoginDialog::onNetworkError(uint32_t errorNumber,
                                 const QString& errorText)
{
    qWarning() << "Net error:" << errorNumber << errorText;
    setError(tr("Network error: %1").arg(errorText));
    ui->loginInProgressLabel->hide();
}

void LoginDialog::onHttpError(uint32_t errorNumber,
                              const QString& errorText)
{
    setError(tr("HTTP error %1: %2").arg(errorNumber).arg(errorText));
    ui->loginInProgressLabel->hide();
}

void LoginDialog::setError (const QString& errorStr)
{
    ui->domain_lineEdit->setEnabled(true);
    ui->username_lineEdit->setEnabled(true);
    ui->password_lineEdit->setEnabled(true);
    ui->login_pushButton->setEnabled(true);
    _tokenMode->setEnabled(true);
    ui->login_pushButton->setText(_editingExisting ? tr("Save") : tr("Sign in"));

    ui->error_label->setText (errorStr);
}

} /* namespace Mattermost */

