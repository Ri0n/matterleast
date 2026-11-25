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
#include "plugins/PluginManager.h"
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
#include "channel-tree/ChannelTree.h"
#include "channel-tree/SidebarItem.h"
#include "ui/ServersSidebarPage.h"
#include "ui/AccountTrayBadgeMonitor.h"
#include "notifications/AccountNotifications.h"
#include "notifications/NotificationManager.h"
#include "navigation/NavigationUiController.h"
#include "navigation/AppNavigationService.h"
#include "ui/TeamSelectorLabel.h"
#include "backend/types/BackendChannel.h"

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
    void ensureMainWindow();
    void attachAccountPresentation(Backend& backend, bool activate);
    void suspendProfileConnection(Account* account, Backend* backend);
    void populateAccountMenu(QMenu& menu);
    void selectAccount(const QString& accountId, bool presentWindow = false);
    void bindActiveNavigationView();
    void syncViewSidebar();

    // One account domain; the application only owns its Widgets and tray.
    AccountManager accounts;
    PluginManager plugins;
    UserActivityMonitor _activityMonitor;
    MigrationManager migrations;
    std::unique_ptr<QMenu> trayIconMenu;
    std::unique_ptr<QSystemTrayIcon> trayIcon;
    AccountTrayBadgeMonitor trayBadges;
    AccountNotifications accountNotifications;
    std::unique_ptr<MainWindow> mainWindow;
    QSet<Backend*> closingAccounts;
    QPointer<LoginDialog> loginDialog;
    TrayWindowController trayWindow;
};

inline MattermostApplication::MattermostApplication (int& argc, char *argv[])
:QApplication (argc, argv)
,accounts(this)
,plugins(accounts, this)
,_activityMonitor(accounts, this)
,migrations(accounts, this)
,trayIconMenu (std::make_unique<QMenu> (nullptr))
,trayIcon (std::make_unique<QSystemTrayIcon> (IconUtils::applicationIcon(), nullptr))
,trayBadges(*trayIcon)
,accountNotifications(NotificationManager::instance(*trayIcon), plugins,
    [this](Backend& source, const QString& channelId, const QString& rootId) {
        return mainWindow && NavigationUiController::instance(*mainWindow)
            .isConversationVisible(source, channelId, rootId);
    },
    [this]() -> QWidget* { return mainWindow.get(); })
{
    QGuiApplication::setApplicationDisplayName(QStringLiteral("MatterLeast"));
    // Match the installed .desktop file for notification portal identity.
    QGuiApplication::setDesktopFileName(
        QStringLiteral("io.github.Ri0n.MatterLeast"));
    QGuiApplication::setWindowIcon(IconUtils::applicationIcon());

    OverlayScrollBarManager::install(*this);

    Config::init ();
    // Plugins are application-scoped, not owned by a Backend or Account.
    plugins.initializePlugins();
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

    // Notification actions intentionally select their originating account
    // and raise the actual top-level window. Passive delivery never does.
    connect(&NotificationManager::instance(*trayIcon), &NotificationManager::activated,
            this, [this](const NotificationTarget& target) {
        if (!target.isValid()) return;
        auto* source = qobject_cast<Backend*>(target.accountSession.data());
        const auto* owner = accounts.accountForBackend(source);
        if (!owner || closingAccounts.contains(source)) return;
        selectAccount(owner->id());
        if (accounts.activeBackend() != source || !mainWindow) return;
        if (!mainWindow->admitsNavigationBackend(*source)) return;
        if (mainWindow->isMinimized())
            mainWindow->showNormal();
        else
            mainWindow->show();
        mainWindow->raise();
        mainWindow->activateWindow();
        if (auto* channel = source->getStorage().getChannelById(target.channelId)) {
            if (channel->team)
                TeamSelectorLabel::activateTeam(mainWindow.get(), channel->team->id);
        }
        AppNavigationService::instance(*source).openPost(target.postId);
    });

    // The domain owns all network lifetimes; one permanent Widgets surface
    // admits and retires account presentations.
    connect(&accounts, &AccountManager::accountConnected, this,
            [this](Account* account, Backend* backend) {
        trayBadges.watch(*backend);
        accountNotifications.watch(*backend);
        attachAccountPresentation(*backend,
                            account->id() == accounts.selectedAccountId());
    });
    connect(&accounts, &AccountManager::accountUnregistered, this,
            [this](Backend* backend) {
        trayBadges.unwatch(backend);
        accountNotifications.unwatch(backend);
        closingAccounts.remove(backend);
    });
    connect(&accounts, &AccountManager::profileSuspensionRequested, this,
            [this](Account* account, Backend* backend) {
        suspendProfileConnection(account, backend);
    });
    connect(&accounts, &AccountManager::activeViewChanged, this,
            [this](const QString&) {
        syncViewSidebar();
        bindActiveNavigationView();
        accounts.persistViewSessions();
    });
    connect(&accounts, &AccountManager::layoutChanged, this,
            [this] {
        syncViewSidebar();
        bindActiveNavigationView();
    });
	trayIconMenu->addAction (tr("Quit"), qApp, &QApplication::quit);
	qApp->setQuitOnLastWindowClosed(false);
}

MattermostApplication::~MattermostApplication()
{
    // A top-level login dialog can outlive its last accepted/rejected signal;
    // close it while AccountManager still owns its pending Backend.
    delete loginDialog.data();
    if (mainWindow) {
        mainWindow->saveState();
        accounts.persistViewSessions();
        for (auto* backend : accounts.registeredBackends())
            if (backend && mainWindow->admitsNavigationBackend(*backend))
                mainWindow->detachNavigationBackend(*backend);
        mainWindow.reset();
    }
}

void MattermostApplication::start()
{
    qInfo("MatterLeast startup: loading saved account catalog and migration state");
    QString error;
    if (!accounts.loadFromOptions(&error)) {
        qWarning() << "Cannot load saved account catalog:" << error;
        ensureMainWindow();
        mainWindow->showStartupError(tr("Saved account settings could not be read. "
                                  "The original configuration has been preserved."));
        mainWindow->showServersSidebar();
        trayWindow.setWindow(mainWindow.get());
        mainWindow->show();
        return;
    }

    // Show persistent accounts immediately. Migrations are asynchronous and
    // must never hold the entire interface hostage to an offline server.
    ensureMainWindow();
    mainWindow->showServersSidebar();
    trayWindow.setWindow(mainWindow.get());
    mainWindow->show();

    migrations.run([this](MigrationManager::Result result) {
        qInfo() << "MatterLeast startup: migration callback result"
                << static_cast<int>(result)
                << "known-accounts" << accounts.allAccounts().size();
        if (result == MigrationManager::Result::UnsupportedVersion) {
            mainWindow->showStartupError(tr("Account settings were written by a newer "
                "version of MatterLeast. No migration was attempted."));
            return;
        }
        if (accounts.empty()) {
            if (result == MigrationManager::Result::Unavailable)
                mainWindow->showStartupError(tr("The previous session could not be "
                    "migrated. Original settings were preserved."));
            // The Servers tab is usable without saved accounts. The user
            // opens credentials explicitly via Add server, not at startup.
            return;
        }
        // Restore *every* account admitted by the selected profile, including
        // those hidden by its current view. Existing authenticated Backends
        // are left untouched, e.g. after the v1 credential migration.
        accounts.restoreActiveProfile();
        if (accounts.activeBackend())
            mainWindow->showChannelsSidebar();
    });
}

void MattermostApplication::ensureMainWindow()
{
    if (mainWindow) return;

    mainWindow = std::make_unique<MainWindow>(nullptr, *trayIcon);
    mainWindow->setWindowIcon(IconUtils::applicationIcon());
    mainWindow->setWindowTitle(applicationDisplayName());
    const QByteArray geometry = MLOptions::instance()->value<QByteArray>(
        QStringLiteral("geometry"));
    if (!geometry.isEmpty())
        mainWindow->restoreGeometry(geometry);
    else
        mainWindow->resize(1200, 800);
    mainWindow->installServersSidebar(accounts);
    plugins.attachMainWindow(*mainWindow);

    // MainWindow saves the active Widgets bookmark first; AccountManager then
    // serializes all View sessions, including those not currently presented.
    connect(qApp, &QApplication::aboutToQuit, this,
            [this] { accounts.persistViewSessions(); });
    connect(mainWindow.get(), &MainWindow::serverOpenRequested, this,
            [this](const QString& id) { selectAccount(id); });
    connect(mainWindow.get(), &MainWindow::viewOpenRequested, this,
            [this](const QString& id) {
        if (!accounts.activateView(id)) return;
        syncViewSidebar();
        bindActiveNavigationView();
        if (mainWindow->backendOrNull()) mainWindow->showChannelsSidebar();
        else mainWindow->showServersSidebar();
    });
    connect(mainWindow.get(), &MainWindow::serverConnectRequested, this,
            [this](const QString& id) {
        if (accounts.selectAccount(id)) accounts.restoreAccount(id);
    });
    connect(mainWindow.get(), &MainWindow::serverSettingsRequested, this,
            [this](const QString& id) { openLoginWindow(id); });
    connect(mainWindow.get(), &MainWindow::serverDisconnectRequested, this,
            [this](const QString& id) { accounts.disconnectAccount(id); });
    connect(mainWindow.get(), &MainWindow::addServerRequested, this,
            [this] { openLoginWindow(); });
    connect(mainWindow.get(), &MainWindow::logoutRequested, this, [this] {
        if (auto* source = mainWindow->backendOrNull()) beginLogout(*source);
    });
    connect(&accounts, &AccountManager::activeBackendChanged, mainWindow.get(),
            [this](Backend*) { syncViewSidebar(); });
    syncViewSidebar();
    bindActiveNavigationView();
    trayWindow.setWindow(mainWindow.get());
}

void MattermostApplication::bindActiveNavigationView()
{
    if (!mainWindow) return;
    if (auto* session = accounts.viewSession(accounts.activeViewId()))
        mainWindow->bindViewNavigation(session);
    else
        mainWindow->unbindViewNavigation();
}

void MattermostApplication::syncViewSidebar()
{
    if (!mainWindow) return;
    QStringList ids;
    QHash<QString, QString> labels;
    QString sorting = QStringLiteral("recent");
    for (const auto& view : accounts.layout().views) {
        if (view.id != accounts.activeViewId()) continue;
        ids = view.accountIds;
        sorting = view.directMessagesSorting;
        break;
    }
    for (const QString& id : ids) {
        const auto* owner = accounts.account(id);
        const auto* record = owner ? owner->record() : nullptr;
        labels.insert(id, record ? record->name : id);
    }
    Backend* selected = accounts.activeBackend();
    if (selected && (closingAccounts.contains(selected)
                    || !mainWindow->admitsNavigationBackend(*selected)))
        selected = nullptr;
    mainWindow->selectPresentationBackend(selected);
    mainWindow->setVisibleAccounts(ids, labels, sorting);
}

void MattermostApplication::selectAccount(const QString& id, bool presentWindow)
{
    if (!accounts.selectAccount(id)) return;
    if (mainWindow) {
        syncViewSidebar();
        bindActiveNavigationView();
        if (mainWindow->backendOrNull())
            mainWindow->showChannelsSidebar();
        else
            mainWindow->showServersSidebar();
        trayWindow.setWindow(mainWindow.get());
    }
    if (presentWindow) trayWindow.showWindow();
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

void MattermostApplication::attachAccountPresentation(Backend& backend, bool activate)
{
    ensureMainWindow();
    if (!accounts.accountForBackend(&backend)
        || !mainWindow->attachNavigationBackend(backend)) {
        qCritical("Unable to attach account presentation");
        return;
    }
    connect(&backend, &Backend::authenticationExpired, this,
            [this, owner = &backend] { beginLogout(*owner, true); });
    syncViewSidebar();
    bindActiveNavigationView();
    if (activate) {
        mainWindow->showChannelsSidebar();
        mainWindow->show();
    }
}

void MattermostApplication::openLoginWindow(const QString& recoveryAccountId,
                                            const QString& suggestedServer,
                                            const QString& suggestedLogin)
{
    // User-triggered login wins over any outstanding legacy keyring lookup,
    // HTTP identity check or asynchronous migration attempt.
    if (migrations.isRunning())
        migrations.cancel();
    if (!mainWindow && !closingAccounts.isEmpty())
        return;
    if (loginDialog) {
        loginDialog->raise();
        loginDialog->activateWindow();
        return;
    }

    if (recoveryAccountId.isEmpty()) {
        // First/additional server: the dialog only collects credentials. No
        // network operation or artificial Backend belongs to the modal form.
        // Once accepted it disappears and the Servers tree shows progress.
        auto* dialog = new LoginDialog(mainWindow.get(),
                                       suggestedServer, suggestedLogin);
        dialog->setWindowModality(Qt::WindowModal);
        loginDialog = dialog;
        connect(dialog, &QDialog::rejected, this, [this, dialog] {
            if (loginDialog == dialog) loginDialog = nullptr;
        });
        connect(dialog, &QDialog::accepted, this, [this, dialog] {
            const BackendLoginData credentials = dialog->submittedCredentials();
            if (loginDialog == dialog) loginDialog = nullptr;
            accounts.beginNewServerLogin(credentials, dialog->displayName());
        });
        dialog->open();
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
        if (!item->backend()) {
            // An explicit Sign in action takes priority over any pending
            // verification *or scheduled VPN reconnect*. Cancel both so the
            // automatic path cannot race the credential dialog.
            accounts.cancelPendingRestore(
                recoveryAccountId, AccountConnectionState::AuthenticationRequired);
        }
    }

    Backend* candidate = accounts.createLoginBackend();
    // Migrations may suggest old non-secret login coordinates. They never
    // authenticate or access keychain from the Widgets credential editor.
    const bool lockIdentity = !fixedServer.isEmpty() && !fixedLogin.isEmpty();
    auto* dialog = new LoginDialog(mainWindow.get(), *candidate,
        lockIdentity ? fixedServer : suggestedServer,
        lockIdentity ? fixedLogin : suggestedLogin, lockIdentity);
    loginDialog = dialog;
    if (Account* existing = accounts.account(recoveryAccountId)) {
        if (const auto* record = existing->record())
            dialog->configureExistingAccount(
                record->name, record->serverUserId,
                existing->state() == AccountConnectionState::Connected);
    }
    if (!mainWindow)
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
            trayWindow.setWindow(mainWindow.get());
    });
    connect(dialog, &QDialog::accepted, this,
            [this, dialog, candidate, adopted, recoveryAccountId] {
        loginDialog = nullptr;
        const QString updatedLabel = dialog->displayName();
        auto saveAlias = [this, &updatedLabel](const QString& accountId) {
            if (updatedLabel.isEmpty()) return;
            WorkspaceLayout updated = accounts.layout();
            for (auto& record : updated.accounts)
                if (record.id == accountId)
                    record.name = updatedLabel;
            QString error;
            if (!accounts.applyLayoutEdits(updated, &error))
                qWarning() << "Server alias update failed:" << error;
        };
        if (!dialog->submittedNewCredentials()) {
            saveAlias(recoveryAccountId);
            return; // Metadata-only edit: candidate is safely discarded.
        }
        const QString host = QUrl(candidate->serverDomain()).host();
        const QString display = updatedLabel.isEmpty()
            ? host + QStringLiteral(" — ") + candidate->getLoginUser().username
            : updatedLabel;

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
        saveAlias(item->id());
        selectAccount(item->id());
        if (mainWindow) {
            trayWindow.setWindow(mainWindow.get());
            mainWindow->show();
        }
    });
    dialog->open();
}

void MattermostApplication::suspendProfileConnection(Account* item, Backend* backend)
{
    if (!item || !backend || item->backend() != backend
        || closingAccounts.contains(backend))
        return;
    closingAccounts.insert(backend);
    accountNotifications.unwatch(backend);
    trayBadges.unwatch(backend);
    const QPointer<Account> itemGuard(item);
    const QPointer<Backend> backendGuard(backend);
    // Retire dependent widgets before the manager resets Storage, after
    // the triggering menu/signal stack unwinds. The window survives.
    QMetaObject::invokeMethod(this, [this, itemGuard, backendGuard] {
        if (!backendGuard) return;
        if (mainWindow) mainWindow->detachNavigationBackend(*backendGuard);
        if (itemGuard)
            accounts.finishProfileSuspension(itemGuard.data(), backendGuard.data());
        syncViewSidebar();
    }, Qt::QueuedConnection);
}

void MattermostApplication::beginLogout(Backend& backend, bool authenticationExpired)
{
    if (closingAccounts.contains(&backend)) return;
    Account* item = accounts.accountForBackend(&backend);
    if (!item) return;
    closingAccounts.insert(&backend);
    accountNotifications.unwatch(&backend);
    trayBadges.unwatch(&backend);
    const QPointer<Account> guard(item);
    const QPointer<Backend> backendGuard(&backend);
    accounts.prepareStop(item, authenticationExpired);

    QMetaObject::invokeMethod(this, [this, guard, backendGuard, authenticationExpired] {
        if (!backendGuard) return;
        if (mainWindow) mainWindow->detachNavigationBackend(*backendGuard);
        if (guard) accounts.finishStop(guard.data(), authenticationExpired);
        else closingAccounts.remove(backendGuard.data());
        syncViewSidebar();
    }, Qt::QueuedConnection);
}

inline void MattermostApplication::showWindow()
{
    if (!trayWindow.window()) {
        if (accounts.empty())
            openLoginWindow();
        else {
            ensureMainWindow();
            trayWindow.setWindow(mainWindow.get());
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
