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
#include <QDialog>
#include <QVBoxLayout>
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
#include "backend/AccountManager.h"
#include "migrations/MigrationManager.h"
#include "backend/Account.h"
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
#include "ui/ServersSidebarPage.h"
#include "ui/AccountTrayBadgeModel.h"

namespace Mattermost {

class MattermostApplication: public QApplication {
public:
    MattermostApplication(int& argc, char* argv[]);
    ~MattermostApplication() override;

    void start();
    void openLoginWindow(const QString& recoveryAccountId = {},
                         const QString& suggestedServer = {},
                         const QString& suggestedLogin = {});
    void showWindow();
    void toggleShowWindow();
    void beginLogout(Backend& backend, bool authenticationExpired = false);

private:
    void ensureWorkspaceShell();
    void addAccountWorkspace(Backend& backend, bool activate);
    void suspendProfileConnection(Account* account, Backend* backend);
    void populateAccountMenu(QMenu& menu);
    void updateWindowTitle();
    void updateTrayIcon();
    void selectAccount(const QString& accountId, bool presentWindow = false);
    void bindActiveNavigationView();

    // One account domain; the application only owns its Widgets and tray.
    AccountManager accounts;
    UserActivityMonitor _activityMonitor;
    MigrationManager migrations;
    std::unique_ptr<QMenu> trayIconMenu;
    std::unique_ptr<QSystemTrayIcon> trayIcon;
    AccountTrayBadgeModel trayBadges;
    std::unique_ptr<AccountWorkspaceStack> workspaceShell;
    AccountOverviewPage* overviewPage = nullptr;
    QPointer<QDialog> offlineServersDialog;
    QHash<Backend*, MainWindow*> accountViews; // Presentation only.
    QSet<Backend*> closingAccounts;
    QPointer<LoginDialog> loginDialog;
    TrayWindowController trayWindow;
};

inline MattermostApplication::MattermostApplication (int& argc, char *argv[])
:QApplication (argc, argv)
,accounts(this)
,_activityMonitor(accounts, this)
,migrations(accounts, this)
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

    // The domain owns all network lifetimes; the app creates/removes only
    // account-specific Widgets when the domain reports a connected session.
    connect(&accounts, &AccountManager::accountConnected, this,
            [this](Account* account, Backend* backend) {
        addAccountWorkspace(*backend,
                            account->id() == accounts.selectedAccountId());
    });
    connect(&accounts, &AccountManager::accountUnregistered, this,
            [this](Backend* backend) { closingAccounts.remove(backend); });
    connect(&accounts, &AccountManager::profileSuspensionRequested, this,
            [this](Account* account, Backend* backend) {
        suspendProfileConnection(account, backend);
    });
    connect(&accounts, &AccountManager::activeViewChanged, this,
            [this](const QString&) { bindActiveNavigationView(); });
    connect(&accounts, &AccountManager::layoutChanged, this,
            [this] { bindActiveNavigationView(); });
	trayIconMenu->addAction (tr("Quit"), qApp, &QApplication::quit);
	qApp->setQuitOnLastWindowClosed(false);
}

MattermostApplication::~MattermostApplication()
{
    // A top-level login dialog can outlive its last accepted/rejected signal;
    // close it while AccountManager still owns its pending Backend.
    delete loginDialog.data();
}

void MattermostApplication::start()
{
    QString error;
    if (!accounts.loadFromOptions(&error)) {
        qWarning() << "Cannot load saved account catalog:" << error;
        ensureWorkspaceShell();
        overviewPage->setError(tr("Saved account settings could not be read. "
                                  "The original configuration has been preserved."));
        workspaceShell->showOverview();
        trayWindow.setWindow(workspaceShell.get());
        workspaceShell->show();
        return;
    }

    // Show persistent accounts immediately. Migrations are asynchronous and
    // must never hold the entire interface hostage to an offline server.
    ensureWorkspaceShell();
    workspaceShell->showOverview();
    trayWindow.setWindow(workspaceShell.get());
    workspaceShell->show();

    migrations.run([this](MigrationManager::Result result) {
        if (result == MigrationManager::Result::UnsupportedVersion) {
            overviewPage->setError(tr("Account settings were written by a newer "
                "version of MatterLeast. No migration was attempted."));
            return;
        }
        if (accounts.empty()) {
            if (result == MigrationManager::Result::Unavailable)
                overviewPage->setError(tr("The previous session could not be "
                    "migrated. Original settings were preserved."));
            openLoginWindow({}, migrations.legacyServerHint(),
                            migrations.legacyLoginHint());
            return;
        }
        // Restore *every* account admitted by the selected profile, including
        // those hidden by its current view. Existing authenticated Backends
        // are left untouched, e.g. after the v1 credential migration.
        accounts.restoreActiveProfile();
        if (accounts.activeBackend())
            workspaceShell->activateWorkspace(accounts.selectedAccountId());
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

    overviewPage = new AccountOverviewPage(accounts.listModel(),
                                            workspaceShell.get());
    workspaceShell->setOverviewPage(overviewPage);
    connect(overviewPage, &AccountOverviewPage::accountSelected, this,
            [this](const QString& id) { selectAccount(id); });
    connect(overviewPage, &AccountOverviewPage::retryRequested, this,
            [this](const QString& id) {
        // Keyboard selection of an overview row need not have emitted clicked().
        // Treat Connect as an explicit profile activation before retrying.
        if (accounts.selectAccount(id))
            accounts.restoreAccount(id);
    });
    connect(overviewPage, &AccountOverviewPage::signInRequested, this,
            [this](const QString& id) {
        // Even manual reauthentication must activate the owning profile.
        // The overview's selected row is not itself a connection policy.
        if (accounts.selectAccount(id))
            openLoginWindow(id);
    });
    connect(overviewPage, &AccountOverviewPage::addServerRequested, this,
            [this] { openLoginWindow(); });
    connect(overviewPage, &AccountOverviewPage::manageServersRequested, this,
            [this] {
        if (offlineServersDialog) {
            offlineServersDialog->show();
            offlineServersDialog->raise();
            offlineServersDialog->activateWindow();
            return;
        }
        // The Servers sidebar is attached to a connected Backend's Widgets
        // presentation. The very same QtCore-backed editor must also work
        // while every account is offline, without constructing a fake Backend.
        auto* dialog = new QDialog(workspaceShell.get());
        offlineServersDialog = dialog;
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->setWindowTitle(tr("Manage servers and views"));
        dialog->resize(470, 570);
        auto* layout = new QVBoxLayout(dialog);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* page = new ServersSidebarPage(accounts, dialog);
        layout->addWidget(page);
        connect(page, &ServersSidebarPage::accountOpenRequested, this,
                [this, dialog](const QString& id) {
            selectAccount(id);
            dialog->close();
        });
        connect(page, &ServersSidebarPage::viewOpenRequested, this,
                [this, dialog](const QString& id) {
            if (accounts.activateView(id))
                selectAccount(accounts.selectedAccountId());
            dialog->close();
        });
        connect(page, &ServersSidebarPage::addServerRequested, this,
                [this, dialog] {
            dialog->close();
            openLoginWindow();
        });
        connect(page, &ServersSidebarPage::signInRequested, this,
                [this, dialog](const QString& id) {
            if (accounts.selectAccount(id)) {
                dialog->close();
                openLoginWindow(id);
            }
        });
        connect(page, &ServersSidebarPage::retryRequested, this,
                [this](const QString& id) {
            if (accounts.selectAccount(id)) accounts.restoreAccount(id);
        });
        dialog->show();
    });

    connect(this, &QCoreApplication::aboutToQuit, workspaceShell.get(),
            [this] {
        if (workspaceShell)
            MLOptions::instance()->setValue(
                QStringLiteral("geometry"), workspaceShell->saveGeometry());
    });
    connect(workspaceShell.get(), &AccountWorkspaceStack::activeWorkspaceChanged,
            this, [this](const QString& id) {
        if (!id.isEmpty() && id != accounts.selectedAccountId())
            accounts.selectAccount(id);
        updateWindowTitle();
    });
    connect(&accounts, &AccountManager::activeBackendChanged,
            workspaceShell.get(), [this](Backend* backend) {
        if (backend && workspaceShell)
            workspaceShell->activateWorkspace(accounts.selectedAccountId());
        updateWindowTitle();
    });
}

void MattermostApplication::bindActiveNavigationView()
{
    // The current Widgets shell still owns a MainWindow per Backend. Binding
    // the same shared view session to two account windows would let either
    // overwrite cross-account tabs with an incomplete single-account snapshot.
    // A merged view must wait for its own multi-account presenter.
    const QString viewId = accounts.activeViewId();
    QString singleAccount;
    for (const auto& view : accounts.layout().views) {
        if (view.id == viewId && view.accountIds.size() == 1) {
            singleAccount = view.accountIds.front();
            break;
        }
    }

    MainWindow* target = nullptr;
    if (!singleAccount.isEmpty()) {
        Account* account = accounts.account(singleAccount);
        if (account && account->backend()
            && !closingAccounts.contains(account->backend()))
            target = accountViews.value(account->backend(), nullptr);
    }
    // A combined view must never receive a single Backend's tab/history
    // updates, even after editing an existing one-server view into a merged one.
    for (MainWindow* page : accountViews) {
        if (page && page != target)
            page->unbindViewNavigation();
    }
    if (target)
        target->bindViewNavigation(accounts.viewSession(viewId), singleAccount);
}

void MattermostApplication::selectAccount(const QString& id,
                                           bool presentWindow)
{
    if (!accounts.selectAccount(id))
        return;
    if (workspaceShell) {
        Account* item = accounts.account(id);
        if (item && item->backend() && !closingAccounts.contains(item->backend()))
            workspaceShell->activateWorkspace(id);
        else
            workspaceShell->showOverview();
        trayWindow.setWindow(workspaceShell.get());
    }
    updateWindowTitle();
    if (presentWindow)
        trayWindow.showWindow();
}

void MattermostApplication::populateAccountMenu(QMenu& menu)
{
    menu.clear();
    for (Account* item : accounts.allAccounts()) {
        const auto* record = item->record();
        if (!record)
            continue;
        const auto state = item->state();
        const QString marker = state == AccountConnectionState::Connected
            ? QString() : QStringLiteral(" (") +
              (state == AccountConnectionState::Connecting ? tr("connecting")
               : state == AccountConnectionState::AuthenticationRequired
                   ? tr("sign-in required") : tr("offline")) + QLatin1Char(')');
        QAction* action = menu.addAction(record->name + marker);
        action->setCheckable(true);
        action->setChecked(item->id() == accounts.selectedAccountId());
        connect(action, &QAction::triggered, this,
                [this, id = item->id()] { selectAccount(id, true); });
    }
    if (!accounts.empty())
        menu.addSeparator();
    menu.addAction(tr("Add server…"), this,
                   [this] { openLoginWindow(); });
}

void MattermostApplication::updateWindowTitle()
{
    if (!workspaceShell)
        return;
    Backend* selected = accounts.activeBackend();
    MainWindow* page = accountViews.value(selected, nullptr);
    if (!selected || !page) {
        workspaceShell->setWindowTitle(applicationDisplayName());
        return;
    }
    workspaceShell->setWindowTitle(page->windowTitle()
        + QStringLiteral(" — ") + QUrl(selected->serverDomain()).host());
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
    Account* item = accounts.accountForBackend(&backend);
    if (!item)
        return;

    auto* page = new MainWindow(nullptr, *trayIcon, backend);
    const QString id = item->id();
    if (!workspaceShell->addWorkspace(id, page)) {
        delete page;
        qCritical("Unable to install account workspace");
        return;
    }
    accountViews.insert(&backend, page);
    trayBadges.update(&backend, 0, false);

    page->installServersSidebar(accounts);
    connect(page, &MainWindow::serverOpenRequested, this,
            [this](const QString& id) {
        selectAccount(id);
        Account* selected = accounts.account(id);
        if (selected && selected->backend()) {
            if (MainWindow* target = accountViews.value(selected->backend(), nullptr))
                target->showChannelsSidebar();
        }
    });
    connect(page, &MainWindow::viewOpenRequested, this,
            [this](const QString& id) {
        if (!accounts.activateView(id) || !workspaceShell) return;
        Account* selected = accounts.account(accounts.selectedAccountId());
        if (selected && selected->backend()
            && !closingAccounts.contains(selected->backend())) {
            workspaceShell->activateWorkspace(selected->id());
            if (MainWindow* target = accountViews.value(selected->backend(), nullptr))
                target->showChannelsSidebar();
        } else {
            workspaceShell->showOverview();
        }
        bindActiveNavigationView();
        updateWindowTitle();
    });
    connect(page, &MainWindow::addServerRequested, this,
            [this] { openLoginWindow(); });
    connect(page, &MainWindow::serverSignInRequested, this,
            [this](const QString& id) {
        if (accounts.selectAccount(id)) openLoginWindow(id);
    });
    connect(page, &MainWindow::serverConnectRequested, this,
            [this](const QString& id) {
        if (accounts.selectAccount(id)) accounts.restoreAccount(id);
    });
        connect(page, &MainWindow::trayBadgeStateChanged, this,
            [this, owner = &backend](uint32_t count, bool hasUnreadChannel) {
        trayBadges.update(owner, count, hasUnreadChannel);
        updateTrayIcon();
    });
    connect(page, &QWidget::windowTitleChanged, this,
            [this, owner = &backend](const QString&) {
        if (accounts.activeBackend() == owner)
            updateWindowTitle();
    });
    connect(page, &MainWindow::logoutRequested, this,
            [this, owner = &backend] { beginLogout(*owner); });
    connect(page, &MainWindow::presentAccountRequested, this,
            [this, id] { selectAccount(id); });
    connect(&backend, &Backend::authenticationExpired, this,
            [this, owner = &backend] { beginLogout(*owner, true); });

    page->installRealtimeUiSync();
    // A backend arriving after view activation must adopt its retained view
    // session before the startup channel snapshot is replayed.
    bindActiveNavigationView();
    if (activate)
        workspaceShell->activateWorkspace(id);
    trayWindow.setWindow(workspaceShell.get());
    updateWindowTitle();
    updateTrayIcon();
    if (activate)
        workspaceShell->show();
}

void MattermostApplication::openLoginWindow(const QString& recoveryAccountId,
                                            const QString& suggestedServer,
                                            const QString& suggestedLogin)
{
    // User-triggered login wins over any outstanding legacy keyring lookup,
    // HTTP identity check or asynchronous migration attempt.
    if (migrations.isRunning())
        migrations.cancel();
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
        Account* item = accounts.account(recoveryAccountId);
        const auto* record = item ? item->record() : nullptr;
        if (!record)
            return;
        if (record->hasConnectionIdentity()) {
            fixedServer = record->serverBaseUrl;
            fixedLogin = record->loginName;
        }
        if (item->pendingBackend()) {
            accounts.cancelPendingRestore(
                recoveryAccountId, AccountConnectionState::AuthenticationRequired);
        }
    }

    Backend* candidate = accounts.createLoginBackend();
    // Migrations may suggest old non-secret login coordinates. They never
    // authenticate or access keychain from the Widgets credential editor.
    const bool lockIdentity = !fixedServer.isEmpty() && !fixedLogin.isEmpty();
    auto* dialog = new LoginDialog(nullptr, *candidate,
        lockIdentity ? fixedServer : suggestedServer,
        lockIdentity ? fixedLogin : suggestedLogin, lockIdentity);
    loginDialog = dialog;
    if (!workspaceShell)
        trayWindow.setWindow(dialog);

    auto adopted = std::make_shared<bool>(false);
    connect(dialog, &QObject::destroyed, this, [this, candidate, adopted] {
        if (!*adopted)
            accounts.discardCandidate(candidate);
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
        const QString host = QUrl(candidate->serverDomain()).host();
        const QString display = host + QStringLiteral(" — ")
            + candidate->getLoginUser().username;

        Account* item = accounts.adoptAuthenticated(
            candidate, dialog->authenticatedLoginName(), display,
            recoveryAccountId);
        if (!item) {
            // The same remote identity may already be connected. Select its
            // durable account rather than duplicating Backend/Storage.
            Backend* existing = accounts.findAccount(
                candidate->networkSession().host(), candidate->getLoginUser().id);
            if (existing) {
                if (Account* existingAccount = accounts.accountForBackend(existing))
                    selectAccount(existingAccount->id(), true);
            } else {
                qWarning("Authenticated account could not be registered");
            }
            return; // Destroyed dialog discards the unadopted candidate.
        }
        *adopted = true;
        selectAccount(item->id());
        if (workspaceShell) {
            trayWindow.setWindow(workspaceShell.get());
            workspaceShell->show();
        }
    });
    dialog->open();
}

void MattermostApplication::suspendProfileConnection(Account* item,
                                                       Backend* backend)
{
    if (!item || !backend || item->backend() != backend)
        return;
    // Explicit logout/authentication expiry already owns teardown. When it
    // completes, AccountManager reconciles the latest requested profile.
    if (closingAccounts.contains(backend))
        return;

    closingAccounts.insert(backend);
    const QString accountId = item->id();
    QPointer<Account> itemGuard(item);
    QPointer<Backend> backendGuard(backend);
    // Do not let QStackedWidget::removeWidget() automatically select another
    // account when an outgoing page happens to be current. Display the
    // neutral overview until the target view's account is ready.
    if (workspaceShell && workspaceShell->activeAccountId() == accountId)
        workspaceShell->showOverview();
    if (MainWindow* page = accountViews.value(backend, nullptr))
        page->prepareForLogout();

    // Crucially, a suspended profile is not a logout. The manager already
    // stopped its transports, but must not clear Storage until this account's
    // Widgets (including thread panes) have been released.
    QMetaObject::invokeMethod(this,
        [this, itemGuard, backendGuard, accountId] {
        if (workspaceShell) {
            QWidget* view = workspaceShell->takeWorkspace(accountId);
            delete view;
        }
        if (backendGuard) {
            accountViews.remove(backendGuard.data());
            trayBadges.remove(backendGuard.data());
        }
        updateTrayIcon();
        if (workspaceShell && accounts.selectedAccountId() == accountId)
            workspaceShell->showOverview();

        if (itemGuard && backendGuard)
            accounts.finishProfileSuspension(itemGuard.data(),
                                             backendGuard.data());
        updateWindowTitle();
    }, Qt::QueuedConnection);
}

void MattermostApplication::beginLogout(Backend& backend,
                                        bool authenticationExpired)
{
    if (closingAccounts.contains(&backend) || !accountViews.contains(&backend))
        return;
    Account* item = accounts.accountForBackend(&backend);
    if (!item)
        return;

    closingAccounts.insert(&backend);
    const QString id = item->id();
    QPointer<Account> guard(item);
    accounts.prepareStop(item, authenticationExpired);
    if (MainWindow* page = accountViews.value(&backend, nullptr))
        page->prepareForLogout();

    // Queued teardown keeps QAction callbacks out of destroyed QWidget trees.
    QMetaObject::invokeMethod(this,
                              [this, owner = &backend, guard, id,
                               authenticationExpired] {
        QWidget* view = workspaceShell
            ? workspaceShell->takeWorkspace(id) : nullptr;
        accountViews.remove(owner);
        delete view;
        trayBadges.remove(owner);
        updateTrayIcon();
        if (workspaceShell && accounts.selectedAccountId() == id)
            workspaceShell->showOverview();
        if (guard)
            accounts.finishStop(guard.data(), authenticationExpired);
        else
            closingAccounts.remove(owner);
        updateWindowTitle();
    }, Qt::QueuedConnection);
}

inline void MattermostApplication::showWindow()
{
    if (!trayWindow.window()) {
        if (accounts.empty())
            openLoginWindow();
        else {
            ensureWorkspaceShell();
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
