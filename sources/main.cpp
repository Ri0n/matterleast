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

#include <memory>
#include <utility>
#include <QApplication>
#include <QAction>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QCryptographicHash>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QStandardPaths>
#include <QTimer>
#include <QSaveFile>
#include <QFileInfo>
#include <QPointer>
#include <QHash>
#include <QSet>
#include <QUrl>

#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
#include <QOperatingSystemVersion>
#endif

#if defined(Q_OS_WIN) && !defined(MATTERLEAST_UPDATES_DISABLED)
#include "update/WindowsUpdateController.h"
#include "update/UpdateTrustPolicy.h"
#include "options/MLOptions.h"
#include <QAction>
#include <QSignalBlocker>
#endif

#include "login/LoginDialog.h"
#include "mainwindow.h"
#include "backend/Backend.h"
#include "backend/AccountSessionManager.h"
#include "backend/AccountIdentity.h"
#include "backend/SessionTokenStore.h"
#include "backend/types/BackendLoginData.h"
#include "backend/UserActivityMonitor.h"
#include "backend/CustomEmojiService.h"
#include "options/MLOptions.h"
#include "config/Config.h"
#include "diagnostics/LoggingConfiguration.h"
#include "ui/IconUtils.h"
#include "ui/ApplicationFont.h"
#include "ui/OverlayScrollBarManager.h"
#include "ui/TrayWindowController.h"
#include "ui/AccountWorkspaceStack.h"
#include "ui/AccountOverviewPage.h"
#include "workspaces/AccountRegistry.h"
#include "ui/AccountTrayBadgeModel.h"

namespace Mattermost {

class MattermostApplication: public QApplication {
public:
    MattermostApplication(int& argc, char* argv[]);
    ~MattermostApplication() override;

    void start();
    void openLoginWindow(const QString& recoveryAccountId = {});
    void showWindow();
    void toggleShowWindow();
    void beginLogout(Backend& backend, bool authenticationExpired = false);
private:
    enum class ConnectionStatus {
        Disconnected, Connecting, Connected, Unavailable, AuthenticationRequired
    };

    void ensureWorkspaceShell();
    void addAccountWorkspace(Backend& backend, bool activate);
    void populateAccountMenu(QMenu& menu);
    void updateWindowTitle();
    void updateTrayIcon();
    void discardFinishedAccount(Backend* backend);
    void saveAccounts();
    void refreshOverview();
    void selectAccount(const QString& accountId, bool presentWindow = false);
    void restoreAccount(const QString& accountId);
    void cancelPendingRestore(const QString& accountId, ConnectionStatus status);
    QString idForBackend(const Backend* backend) const;

    // AccountRegistry owns durable account records; AccountSessionManager owns
    // only optional live Backend QObjects. Neither owns the other's lifetime.
    AccountSessionManager accounts;
    UserActivityMonitor _activityMonitor;
    AccountRegistry accountRegistry;
    std::unique_ptr<QMenu> trayIconMenu;
    std::unique_ptr<QSystemTrayIcon> trayIcon;
    AccountTrayBadgeModel trayBadges;
    std::unique_ptr<AccountWorkspaceStack> workspaceShell;
    AccountOverviewPage* overviewPage = nullptr; // QWidget child of workspaceShell
    QHash<Backend*, MainWindow*> accountViews;
    QHash<QString, QPointer<Backend>> liveByAccountId;
    QHash<QString, QPointer<Backend>> pendingByAccountId;
    QHash<QString, ConnectionStatus> connectionStatuses;
    QSet<Backend*> closingAccounts;
    QPointer<LoginDialog> loginDialog;
    TrayWindowController trayWindow;
    QString selectedAccountId;
    bool invalidAccountCatalog = false;
    bool suppressNextAutomaticLogin = false;
};

inline MattermostApplication::MattermostApplication (int& argc, char *argv[])
:QApplication (argc, argv)
,accounts(this)
,_activityMonitor(accounts, this)
,trayIconMenu (std::make_unique<QMenu> (nullptr))
,trayIcon (std::make_unique<QSystemTrayIcon> (IconUtils::applicationIcon(), nullptr))
{
    QGuiApplication::setApplicationDisplayName(QStringLiteral("MatterLeast"));
    // Match the installed .desktop file for notification portal identity.
    QGuiApplication::setDesktopFileName(
        QStringLiteral("io.github.Ri0n.MatterLeast"));
    QGuiApplication::setWindowIcon(IconUtils::applicationIcon());

    OverlayScrollBarManager::install(*this);

    Config::init ();
	trayIcon->setToolTip(applicationDisplayName());
	trayIcon->setContextMenu (trayIconMenu.get());
	trayIcon->show();

	connect (trayIcon.get(), &QSystemTrayIcon::messageClicked, this, &MattermostApplication::showWindow);

	connect (trayIcon.get(), &QSystemTrayIcon::activated, [this] (QSystemTrayIcon::ActivationReason reason) {
		if (reason == QSystemTrayIcon::Trigger) {
			toggleShowWindow ();
		}
	});

	trayIconMenu->addAction (tr("Open MatterLeast"), this, &MattermostApplication::showWindow);
#if defined(Q_OS_WIN) && !defined(MATTERLEAST_UPDATES_DISABLED)
    auto* updater = new WindowsUpdateController(this);
    auto* updates = trayIconMenu->addMenu(tr("Updates"));
    updates->addAction(tr("Check for updates"), updater, &WindowsUpdateController::checkNow);
    auto* restart = updates->addAction(tr("Update and restart"), updater,
                                      &WindowsUpdateController::startUpdateAndRestart);
    restart->setEnabled(updater->canApply());
    auto* automatic = updates->addAction(tr("Check automatically"));
    automatic->setCheckable(true);
    auto* automaticOption = MLOptions::instance()->optionObject<bool>(
        QStringLiteral("updates/automatic"), true);
    automatic->setChecked(automaticOption->value().toBool());
    connect(automatic, &QAction::toggled, automaticOption,
            [automaticOption](bool checked) { automaticOption->setValue(checked); });
    connect(automaticOption, &MLOptionObject::changed, automatic,
            [automatic](const QVariant& value) {
        const QSignalBlocker blocker(automatic);
        automatic->setChecked(value.toBool());
    });
    auto* nightly = updates->addAction(tr("Use nightly update channel"));
    nightly->setCheckable(true);
    auto* channelOption = MLOptions::instance()->optionObject<QString>(
        QStringLiteral("updates/channel"), defaultUpdateChannelName());
    nightly->setChecked(channelOption->value().toString() == QLatin1String("nightly"));
    connect(nightly, &QAction::toggled, updater, [updater, channelOption](bool checked) {
        channelOption->setValue(checked ? QStringLiteral("nightly")
                                        : QStringLiteral("stable"));
        updater->checkNow();
    });
    connect(channelOption, &MLOptionObject::changed, nightly,
            [nightly](const QVariant& value) {
        const QSignalBlocker blocker(nightly);
        nightly->setChecked(value.toString() == QLatin1String("nightly"));
    });
    connect(updater, &WindowsUpdateController::stateChanged, this,
            [restart, updater](WindowsUpdateController::State) {
        restart->setEnabled(updater->canApply());
    });
    // The persistent sidebar banner is the unobtrusive update notification.
    // Do not also raise a system-tray popup for background checks.
    connect(updater, &WindowsUpdateController::errorOccurred, this,
            [this](const QString& error) {
        trayIcon->showMessage(tr("MatterLeast update failed"), error,
                              QSystemTrayIcon::Warning, 10000);
    });
#endif
    QMenu* servers = trayIconMenu->addMenu(tr("Servers"));
    connect(servers, &QMenu::aboutToShow, this,
            [this, servers] { populateAccountMenu(*servers); });
	trayIconMenu->addAction (tr("Quit"), qApp, &QApplication::quit);
	qApp->setQuitOnLastWindowClosed(false);
}

MattermostApplication::~MattermostApplication()
{
    // A top-level login dialog can outlive its last accepted/rejected signal;
    // close it while AccountSessionManager still owns its pending Backend.
    delete loginDialog.data();
}

void MattermostApplication::start()
{
    QString error;
    const QByteArray saved = MLOptions::instance()->value<QByteArray>(
        QStringLiteral("workspaces/layout"));
    if (!accountRegistry.load(saved, &error)) {
        invalidAccountCatalog = true;
        qWarning() << "Cannot load saved account catalog:" << error;
        ensureWorkspaceShell();
        overviewPage->setError(tr("Saved account settings could not be read. "
                                  "The original configuration has been preserved."));
        refreshOverview();
        workspaceShell->show();
        trayWindow.setWindow(workspaceShell.get());
        return;
    }

    if (accountRegistry.empty()) {
        // Legacy single-server/keyring restoration remains available once,
        // until an authenticated account is durably added to the registry.
        openLoginWindow();
        return;
    }

    for (const auto& view : accountRegistry.layout().views) {
        if (view.id == accountRegistry.layout().activeViewId
            && !view.accountIds.isEmpty()) {
            selectedAccountId = view.accountIds.first();
            break;
        }
    }
    ensureWorkspaceShell();
    refreshOverview();
    workspaceShell->showOverview();
    trayWindow.setWindow(workspaceShell.get());
    workspaceShell->show();
    // Keyring reads are independent: one unavailable server never holds up
    // presentation or the other accounts' connection attempts.
    QTimer::singleShot(0, this, [this] {
        for (const auto& record : accountRegistry.layout().accounts)
            if (record.hasConnectionIdentity())
                restoreAccount(record.id);
    });
}

void MattermostApplication::ensureWorkspaceShell()
{
    if (workspaceShell)
        return;

    workspaceShell = std::make_unique<AccountWorkspaceStack>();
    workspaceShell->setWindowIcon(IconUtils::applicationIcon());
    workspaceShell->setWindowTitle(applicationDisplayName());
    const QByteArray geometry = MLOptions::instance()->value<QByteArray>(
        QStringLiteral("geometry"));
    if (!geometry.isEmpty())
        workspaceShell->restoreGeometry(geometry);
    else
        workspaceShell->resize(1200, 800);

    overviewPage = new AccountOverviewPage(workspaceShell.get());
    workspaceShell->setOverviewPage(overviewPage);
    connect(overviewPage, &AccountOverviewPage::accountSelected, this,
            [this](const QString& id) { selectAccount(id); });
    connect(overviewPage, &AccountOverviewPage::retryRequested, this,
            &MattermostApplication::restoreAccount);
    connect(overviewPage, &AccountOverviewPage::signInRequested, this,
            [this](const QString& id) { openLoginWindow(id); });
    connect(overviewPage, &AccountOverviewPage::addServerRequested, this,
            [this] { openLoginWindow(); });

    connect(this, &QCoreApplication::aboutToQuit, workspaceShell.get(),
            [this] {
        if (workspaceShell) {
            MLOptions::instance()->setValue(
                QStringLiteral("geometry"), workspaceShell->saveGeometry());
        }
    });
    connect(workspaceShell.get(), &AccountWorkspaceStack::activeWorkspaceChanged,
            this, [this](Backend* backend) {
        if (backend && accounts.activeBackend() != backend)
            accounts.activate(backend);
        updateWindowTitle();
    });
    connect(&accounts, &AccountSessionManager::activeBackendChanged,
            workspaceShell.get(), [this](Backend* backend) {
        if (backend && workspaceShell
            && selectedAccountId == idForBackend(backend))
            workspaceShell->activateWorkspace(*backend);
        updateWindowTitle();
    });
}

QString MattermostApplication::idForBackend(const Backend* backend) const
{
    for (auto it = liveByAccountId.cbegin(); it != liveByAccountId.cend(); ++it) {
        if (it.value().data() == backend)
            return it.key();
    }
    return {};
}

void MattermostApplication::saveAccounts()
{
    if (invalidAccountCatalog || accountRegistry.empty())
        return;
    const QByteArray layout = accountRegistry.toJson();
    if (!layout.isEmpty())
        MLOptions::instance()->setValue<QByteArray>(
            QStringLiteral("workspaces/layout"), layout);
}

void MattermostApplication::refreshOverview()
{
    if (!overviewPage)
        return;
    QVector<AccountOverviewPage::Entry> entries;
    entries.reserve(accountRegistry.layout().accounts.size());
    for (const auto& record : accountRegistry.layout().accounts) {
        const ConnectionStatus status =
            connectionStatuses.value(record.id, ConnectionStatus::Disconnected);
        QString state;
        switch (status) {
        case ConnectionStatus::Connecting: state = tr("Connecting…"); break;
        case ConnectionStatus::Connected: state = tr("Connected"); break;
        case ConnectionStatus::Unavailable: state = tr("Unavailable — retry"); break;
        case ConnectionStatus::AuthenticationRequired:
            state = tr("Sign-in required"); break;
        case ConnectionStatus::Disconnected: state = tr("Disconnected"); break;
        }
        if (!record.hasConnectionIdentity())
            state = tr("Account setup required");
        entries.append({record.id, record.name,
                        QUrl(record.serverBaseUrl).host(), state});
    }
    overviewPage->setEntries(entries, selectedAccountId);
}

void MattermostApplication::selectAccount(const QString& accountId,
                                           bool presentWindow)
{
    if (!accountRegistry.selectAccount(accountId))
        return;
    selectedAccountId = accountId;
    saveAccounts();
    if (workspaceShell) {
        if (Backend* active = liveByAccountId.value(accountId).data()) {
            if (!closingAccounts.contains(active)) {
                accounts.activate(active);
                workspaceShell->activateWorkspace(*active);
            } else {
                workspaceShell->showOverview();
            }
        } else {
            workspaceShell->showOverview();
        }
        trayWindow.setWindow(workspaceShell.get());
    }
    refreshOverview();
    updateWindowTitle();
    if (presentWindow)
        trayWindow.showWindow();
}

void MattermostApplication::populateAccountMenu(QMenu& menu)
{
    menu.clear();
    for (const auto& record : accountRegistry.layout().accounts) {
        const auto phase = connectionStatuses.value(
            record.id, ConnectionStatus::Disconnected);
        const QString marker = phase == ConnectionStatus::Connected
            ? QString() : QStringLiteral(" (") +
              (phase == ConnectionStatus::Connecting ? tr("connecting")
               : phase == ConnectionStatus::AuthenticationRequired
                   ? tr("sign-in required") : tr("offline")) + QLatin1Char(')');
        QAction* action = menu.addAction(record.name + marker);
        action->setCheckable(true);
        action->setChecked(record.id == selectedAccountId);
        connect(action, &QAction::triggered, this,
                [this, id = record.id] { selectAccount(id, true); });
    }
    if (!accountRegistry.empty())
        menu.addSeparator();
    menu.addAction(tr("Add server…"), this,
                   [this] { openLoginWindow(); });
}

void MattermostApplication::updateWindowTitle()
{
    if (!workspaceShell)
        return;
    Backend* selected = liveByAccountId.value(selectedAccountId).data();
    MainWindow* page = accountViews.value(selected, nullptr);
    if (!selected || !page) {
        workspaceShell->setWindowTitle(applicationDisplayName());
        return;
    }
    const QString host = QUrl(selected->serverDomain()).host();
    workspaceShell->setWindowTitle(page->windowTitle()
                                   + QStringLiteral(" — ") + host);
}

void MattermostApplication::updateTrayIcon()
{
    const auto combined = trayBadges.aggregate();
    trayIcon->setIcon(IconUtils::applicationIcon(
        combined.attentionCount, combined.hasUnreadChannel));
}

void MattermostApplication::addAccountWorkspace(Backend& backend, bool activate)
{
    ensureWorkspaceShell();

    auto* page = new MainWindow(nullptr, *trayIcon, backend);
    if (!workspaceShell->addWorkspace(backend, page)) {
        delete page;
        qCritical("Unable to install account workspace");
        return;
    }
    accountViews.insert(&backend, page);
    trayBadges.update(&backend, 0, false);

    auto* menu = new QMenu(page);
    connect(menu, &QMenu::aboutToShow, this,
            [this, menu] { populateAccountMenu(*menu); });
    page->installAccountSwitcher(menu);
    connect(page, &MainWindow::trayBadgeStateChanged, this,
            [this, account = &backend](uint32_t count, bool hasUnreadChannel) {
        trayBadges.update(account, count, hasUnreadChannel);
        updateTrayIcon();
    });
    connect(page, &QWidget::windowTitleChanged, this,
            [this, account = &backend](const QString&) {
        if (accounts.activeBackend() == account)
            updateWindowTitle();
    });
    connect(page, &MainWindow::logoutRequested, this,
            [this, account = &backend] { beginLogout(*account); });
    connect(page, &MainWindow::presentAccountRequested, this,
            [this, account = &backend] {
        const QString id = idForBackend(account);
        if (!id.isEmpty())
            selectAccount(id);
    });
    connect(&backend, &Backend::authenticationExpired, this,
            [this, account = &backend] { beginLogout(*account, true); });

    page->installRealtimeUiSync();
    if (activate) {
        accounts.activate(&backend);
        workspaceShell->activateWorkspace(backend);
    }
    trayWindow.setWindow(workspaceShell.get());
    refreshOverview();
    updateWindowTitle();
    updateTrayIcon();
    if (activate)
        workspaceShell->show();
}

void MattermostApplication::cancelPendingRestore(const QString& accountId,
                                                  ConnectionStatus status)
{
    QPointer<Backend> candidate = pendingByAccountId.take(accountId);
    if (candidate) {
        if (candidate->loggedIn()) {
            candidate->prepareForLogout();
            candidate->reset();
        } else {
            candidate->cancelPendingLogin();
        }
        accounts.removeBackend(candidate.data());
    }
    connectionStatuses.insert(accountId, status);
    refreshOverview();
}

void MattermostApplication::restoreAccount(const QString& accountId)
{
    const auto* record = accountRegistry.account(accountId);
    if (!record || !record->hasConnectionIdentity() || invalidAccountCatalog)
        return;
    if (Backend* existing = liveByAccountId.value(accountId).data()) {
        if (!closingAccounts.contains(existing))
            selectAccount(accountId);
        return;
    }
    if (pendingByAccountId.value(accountId))
        return;

    // No login dialog and no global "current" server during startup.
    Backend* candidate = accounts.createLoginBackend();
    QPointer<Backend> guarded(candidate);
    pendingByAccountId.insert(accountId, guarded);
    connectionStatuses.insert(accountId, ConnectionStatus::Connecting);
    refreshOverview();

    const QString server = record->serverBaseUrl;
    const QString login = record->loginName;
    SessionTokenStore::read(candidate, server, login,
                            [this, guarded, accountId, server, login](
                                SessionTokenStore::ReadStatus result,
                                const QString& savedToken) {
        Backend* candidate = guarded.data();
        if (!candidate || pendingByAccountId.value(accountId).data() != candidate)
            return;
        if (result != SessionTokenStore::ReadStatus::Found) {
            cancelPendingRestore(
                accountId, result == SessionTokenStore::ReadStatus::NotFound
                    ? ConnectionStatus::AuthenticationRequired
                    : ConnectionStatus::Unavailable);
            return;
        }

        BackendLoginData credentials;
        credentials.domain = server;
        credentials.username = login;
        credentials.token = savedToken;
        candidate->login(credentials, [this, guarded, accountId](
                                         const QString& authenticatedToken) {
            Backend* backend = guarded.data();
            if (!backend || pendingByAccountId.value(accountId).data() != backend)
                return;

            if (authenticatedToken.isEmpty()) {
                const bool transient = backend->lastLoginFailureTransient();
                cancelPendingRestore(accountId,
                    transient ? ConnectionStatus::Unavailable
                              : ConnectionStatus::AuthenticationRequired);
                return;
            }
            const auto* original = accountRegistry.account(accountId);
            if (!original || accountIdentityKey(
                    original->serverBaseUrl, original->serverUserId)
                    != accountIdentityKey(backend->networkSession().host(),
                                           backend->getLoginUser().id)) {
                // Never let a saved token for a different user bind this
                // account's navigation, unread state or disk-cache namespace.
                qWarning("Saved credentials resolved to a different account");
                cancelPendingRestore(accountId,
                                     ConnectionStatus::AuthenticationRequired);
                return;
            }
            pendingByAccountId.remove(accountId);
            if (!accounts.registerAuthenticated(backend)) {
                backend->prepareForLogout();
                backend->reset();
                accounts.removeBackend(backend);
                connectionStatuses.insert(accountId,
                                          ConnectionStatus::AuthenticationRequired);
                refreshOverview();
                return;
            }
            liveByAccountId.insert(accountId, guarded);
            connectionStatuses.insert(accountId, ConnectionStatus::Connected);
            addAccountWorkspace(*backend, accountId == selectedAccountId);
            refreshOverview();
        });
    });
}

void MattermostApplication::openLoginWindow(const QString& recoveryAccountId)
{
    if (!workspaceShell && !closingAccounts.isEmpty())
        return;
    if (loginDialog) {
        loginDialog->raise();
        loginDialog->activateWindow();
        return;
    }

    QString fixedServer;
    QString fixedLogin;
    if (!recoveryAccountId.isEmpty()) {
        const auto* record = accountRegistry.account(recoveryAccountId);
        if (!record)
            return;
        // V1 records have no server identity yet; ask for it once and bind
        // it to this same durable account ID rather than duplicating the row.
        if (record->hasConnectionIdentity()) {
            fixedServer = record->serverBaseUrl;
            fixedLogin = record->loginName;
        }
        if (pendingByAccountId.value(recoveryAccountId))
            cancelPendingRestore(recoveryAccountId,
                                 ConnectionStatus::AuthenticationRequired);
    }

    Backend* candidate = accounts.createLoginBackend();
    // Malformed saved layouts are never overwritten. The user may still
    // sign in manually for this process, without silently replacing the
    // damaged configuration with a new account list.
    const bool allowRestore = !invalidAccountCatalog
        && accountRegistry.empty() && !suppressNextAutomaticLogin;
    suppressNextAutomaticLogin = false;
    auto* dialog = new LoginDialog(nullptr, *candidate, allowRestore,
                                   fixedServer, fixedLogin);
    loginDialog = dialog;
    if (!workspaceShell)
        trayWindow.setWindow(dialog);

    // A pending Backend belongs to the manager. An accepted dialog adopts it
    // only after its identity is registered; a rejected dialog cancels it.
    auto adopted = std::make_shared<bool>(false);
    connect(dialog, &QObject::destroyed, this, [this, candidate, adopted] {
        if (!*adopted) {
            candidate->cancelPendingLogin();
            accounts.removeBackend(candidate);
        }
    });
    connect(dialog, &QDialog::rejected, this, [this, dialog, candidate] {
        candidate->cancelPendingLogin();
        if (loginDialog == dialog)
            loginDialog = nullptr;
        if (trayWindow.window() == dialog)
            trayWindow.setWindow(workspaceShell.get());
    });

    connect(dialog, &QDialog::accepted, this,
            [this, dialog, candidate, adopted, recoveryAccountId] {
        loginDialog = nullptr;
        const QString remoteKey = accountIdentityKey(
            candidate->networkSession().host(), candidate->getLoginUser().id);
        if (!recoveryAccountId.isEmpty()) {
            const auto* record = accountRegistry.account(recoveryAccountId);
            if (!record || (record->hasConnectionIdentity()
                && remoteKey != accountIdentityKey(
                    record->serverBaseUrl, record->serverUserId))) {
                qWarning("Reauthentication returned a different account");
                candidate->reset();
                connectionStatuses.insert(recoveryAccountId,
                                          ConnectionStatus::AuthenticationRequired);
                refreshOverview();
                return;
            }
        }
        if (!accounts.registerAuthenticated(candidate)) {
            Backend* existing = accounts.findAccount(
                candidate->networkSession().host(), candidate->getLoginUser().id);
            candidate->reset();
            if (existing) {
                const QString id = idForBackend(existing);
                if (!id.isEmpty())
                    selectAccount(id, true);
            } else {
                qWarning("Authenticated account could not be registered");
            }
            return;
        }

        AccountRegistry updated = accountRegistry;
        const QString host = QUrl(candidate->serverDomain()).host();
        const QString display = host + QStringLiteral(" — ")
            + candidate->getLoginUser().username;
        QString localId;
        const auto* recoveryRecord = recoveryAccountId.isEmpty() ? nullptr
            : accountRegistry.account(recoveryAccountId);
        if (recoveryRecord && !recoveryRecord->hasConnectionIdentity()) {
            if (updated.resolveUnconfigured(
                    recoveryAccountId, candidate->networkSession().host(),
                    candidate->getLoginUser().id,
                    dialog->authenticatedLoginName()))
                localId = recoveryAccountId;
        } else {
            localId = updated.registerAuthenticated(
                candidate->networkSession().host(), candidate->getLoginUser().id,
                dialog->authenticatedLoginName(), display);
        }
        if (localId.isEmpty()) {
            qWarning("Unable to save the authenticated account record");
            candidate->reset();
            return;
        }
        updated.selectAccount(localId);
        accountRegistry = std::move(updated);
        selectedAccountId = localId;
        saveAccounts();
        *adopted = true;
        liveByAccountId.insert(localId, candidate);
        connectionStatuses.insert(localId, ConnectionStatus::Connected);

        // A successfully authenticated workspace can be shown immediately;
        // the login dialog's destruction must not delete its Backend.
        addAccountWorkspace(*candidate, true);
        if (workspaceShell)
            trayWindow.setWindow(workspaceShell.get());
    });
    dialog->open();
}

void MattermostApplication::beginLogout(Backend& backend,
                                        bool authenticationExpired)
{
    if (closingAccounts.contains(&backend) || !accountViews.contains(&backend))
        return;
    closingAccounts.insert(&backend);
    const QString id = idForBackend(&backend);
    if (!id.isEmpty()) {
        connectionStatuses.insert(id,
            authenticationExpired ? ConnectionStatus::AuthenticationRequired
                                  : ConnectionStatus::Disconnected);
        if (const auto* record = accountRegistry.account(id)) {
            // Do not retry a rejected/explicitly logged-out session token.
            // The deletion job belongs to QApplication, not a dying Backend.
            SessionTokenStore::remove(this, record->serverBaseUrl,
                                      record->loginName);
        }
        refreshOverview();
    }

    backend.prepareForLogout();
    if (MainWindow* page = accountViews.value(&backend, nullptr))
        page->prepareForLogout();

    // The action triggering logout may still be on the deleted page's stack.
    QMetaObject::invokeMethod(this, [this, account = &backend,
                                     id, authenticationExpired] {
        QWidget* view = workspaceShell
            ? workspaceShell->takeWorkspace(*account) : nullptr;
        accountViews.remove(account);
        delete view;
        if (workspaceShell && selectedAccountId == id)
            workspaceShell->showOverview();
        trayBadges.remove(account);
        updateTrayIcon();

        if (authenticationExpired) {
            // The server already rejected this session. No /users/logout
            // request is needed; reset only after destroying all its widgets.
            account->reset();
            QMetaObject::invokeMethod(this, [this, account] {
                discardFinishedAccount(account);
            }, Qt::QueuedConnection);
        } else {
            account->logout([this, account] {
                QMetaObject::invokeMethod(this, [this, account] {
                    discardFinishedAccount(account);
                }, Qt::QueuedConnection);
            });
        }
    }, Qt::QueuedConnection);
}

void MattermostApplication::discardFinishedAccount(Backend* backend)
{
    const QString id = idForBackend(backend);
    closingAccounts.remove(backend);
    if (!id.isEmpty())
        liveByAccountId.remove(id);
    accounts.removeBackend(backend);

    // An explicit logout or 401 affects only one account; its record and
    // offline presentation remain available, even if no Backends survive.
    if (workspaceShell && (!liveByAccountId.value(selectedAccountId)
                           || selectedAccountId == id))
        workspaceShell->showOverview();
    updateWindowTitle();
    refreshOverview();
    updateTrayIcon();
}

inline void MattermostApplication::showWindow()
{
    if (!trayWindow.window()) {
        if (accountRegistry.empty())
            openLoginWindow();
        else {
            ensureWorkspaceShell();
            refreshOverview();
            trayWindow.setWindow(workspaceShell.get());
        }
    }
    trayWindow.showWindow();
}

inline void MattermostApplication::toggleShowWindow()
{
    if (!trayWindow.toggleWindow())
        showWindow();
}

} /* namespace Mattermost */

int main( int argc, char *argv[])
{
    // Install before QApplication and Backend constructors emit diagnostics.
    Mattermost::configureApplicationLogging();
	QCoreApplication::setOrganizationName("matterleast");
	QCoreApplication::setApplicationName("MatterLeast");
#ifdef MATTERLEAST_BUILD_VERSION
    QCoreApplication::setApplicationVersion(QStringLiteral(MATTERLEAST_BUILD_VERSION));
#endif
	QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::Round);

    Mattermost::MattermostApplication app(argc, argv);
#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    // Windows 10 defaults to the windowsvista widget style, which paints
    // light controls even when Qt detects the system's dark palette.
    // Fusion honors both palettes and follows runtime theme changes.
    // Keep the native, dark-mode-aware windows11 style on Windows 11.
    if (QOperatingSystemVersion::current() < QOperatingSystemVersion::Windows11)
        QApplication::setStyle(QStringLiteral("Fusion"));
#endif
    Mattermost::initializeApplicationFont(app);
#ifdef MATTERLEAST_BUILD_ID
    // Embedded at build time. The mutable current.version pointer can be
    // repaired or switched by the launcher, so it is NOT the build identity.
    app.setProperty("matterleast-build-id", QStringLiteral(MATTERLEAST_BUILD_ID));
#endif

    // Lock ownership is per OS user, not per working directory or binary
    // installation. Unlike QSingleApplication, QLockFile checks the PID and
    // can recover after an abnormal process termination.
    const QString stateDir = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    if (!QDir().mkpath(stateDir)) {
        qCritical() << "Cannot initialize single-instance state directory" << stateDir;
        return 1;
    }
    QLockFile instanceLock(QDir(stateDir).filePath(QStringLiteral("instance.lock")));
    const QString endpoint = QStringLiteral("matterleast-")
        + QString::fromLatin1(QCryptographicHash::hash(
            QDir(stateDir).absolutePath().toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    if (!instanceLock.tryLock(200)) {
        if (instanceLock.error() != QLockFile::LockFailedError) {
            qCritical() << "Cannot acquire instance lock:" << instanceLock.error();
            return 1;
        }
        // An existing process owns the application; ask it to raise its window.
        QLocalSocket socket;
        socket.connectToServer(endpoint);
        if (socket.waitForConnected(500)) {
            socket.write("activate");
            socket.waitForBytesWritten(500);
            socket.disconnectFromServer();
        }
        return 0;
    }

    QLocalServer server;
    // Only the lock owner may remove a leftover endpoint from a crashed
    // predecessor. Never unlink a socket owned by a live instance.
    QLocalServer::removeServer(endpoint);
    if (!server.listen(endpoint)) {
        qCritical() << "Cannot listen on the single-instance endpoint:"
                    << server.errorString();
        return 1;
    }
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        while (QLocalSocket* peer = server.nextPendingConnection()) {
            QObject::connect(peer, &QLocalSocket::readyRead, &app, [peer, &app] {
                if (peer->readAll().contains("activate")) {
                    app.showWindow();
                }
            });
            // A client can have sent its entire command before newConnection
            // is delivered, so also inspect already-buffered bytes.
            QTimer::singleShot(0, peer, [peer, &app] {
                if (peer->bytesAvailable() && peer->readAll().contains("activate"))
                    app.showWindow();
            });
            QObject::connect(peer, &QLocalSocket::disconnected,
                             peer, &QLocalSocket::deleteLater);
        }
    });

#ifdef Q_OS_WIN
    // Started by the standalone version switcher: acknowledge startup only
    // after the main event loop has remained healthy for 60 seconds.
    const QStringList args = QCoreApplication::arguments();
    const int probeArg = args.indexOf(QStringLiteral("--matterleast-update-probe-file"));
    if (probeArg >= 0 && probeArg + 1 < args.size()) {
        const QString probe = QFileInfo(args[probeArg + 1]).absoluteFilePath();
        QDir installRoot(QCoreApplication::applicationDirPath());
        if (installRoot.cdUp() && installRoot.cdUp()
            && installRoot.cdUp() && installRoot.cdUp()) {
            const QString staging = installRoot.filePath(QStringLiteral("staging"));
            const QString prefix = QDir(staging).absolutePath() + QLatin1Char('/');
            if (probe.startsWith(prefix, Qt::CaseInsensitive)
                && QFileInfo(probe).fileName().startsWith(QStringLiteral("startup-"))
                && probe.endsWith(QStringLiteral(".ok"))) {
                QTimer::singleShot(60000, &app, [probe] {
                    QSaveFile marker(probe);
                    if (marker.open(QIODevice::WriteOnly)) {
                        marker.write("healthy\\n");
                        marker.commit();
                    }
                });
            }
        }
    }
#endif

    app.start();
    return app.exec();
}
