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
#include <QApplication>
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
#include "backend/UserActivityMonitor.h"
#include "backend/CustomEmojiService.h"
#include "config/Config.h"
#include "ui/IconUtils.h"
#include "ui/OverlayScrollBarManager.h"

namespace Mattermost {

class MattermostApplication: public QApplication {
public:
	MattermostApplication (int& argc, char *argv[]);

	void openLoginWindow ();
	void showWindow ();
	void toggleShowWindow ();
	void reopen ();
private:
	// Members are destroyed in reverse declaration order. Keep Backend alive
	// until every window and tray object that can reference it is gone, and keep
	// the tray menu alive until after QSystemTrayIcon releases its menu pointer.
	Backend								backend;
    UserActivityMonitor                   _activityMonitor;
	std::unique_ptr<QMenu>				trayIconMenu;
	std::unique_ptr<QSystemTrayIcon> 	trayIcon;
	std::unique_ptr<MainWindow>			mainWindow;
	LoginDialog*						loginDialog;
	QWidget*							currentWindow;
};

inline MattermostApplication::MattermostApplication (int& argc, char *argv[])
:QApplication (argc, argv)
,trayIconMenu (std::make_unique<QMenu> (nullptr))
,trayIcon (std::make_unique<QSystemTrayIcon> (IconUtils::applicationIcon(), nullptr))
,currentWindow (nullptr)
{
    QGuiApplication::setApplicationDisplayName(QStringLiteral("MatterLeast"));
    // Match the installed .desktop file for notification portal identity.
    QGuiApplication::setDesktopFileName(
        QStringLiteral("io.github.Ri0n.MatterLeast"));
    QGuiApplication::setWindowIcon(IconUtils::applicationIcon());

    OverlayScrollBarManager::install(*this);
    (void)CustomEmojiService::instance(backend);

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
	trayIconMenu->addAction (tr("Quit"), qApp, &QApplication::quit);
	qApp->setQuitOnLastWindowClosed(false);
}

void MattermostApplication::openLoginWindow ()
{
	loginDialog = new LoginDialog (nullptr, backend);
	loginDialog->open();
	currentWindow = loginDialog;

	connect (loginDialog, &LoginDialog::accepted, [this] {
		//create Main Window and open it, after successful login
		loginDialog = nullptr;
		mainWindow = std::make_unique<MainWindow> (nullptr, *trayIcon, backend);
        mainWindow->installRealtimeUiSync();
		mainWindow->show();
		currentWindow = mainWindow.get();
	});
}

inline void MattermostApplication::showWindow()
{
    if (!currentWindow) return;
    if (currentWindow->isMinimized()) currentWindow->showNormal();
    else if (!currentWindow->isVisible()) currentWindow->show();
    currentWindow->raise();
    currentWindow->activateWindow();
}

inline void MattermostApplication::toggleShowWindow ()
{
	if (!currentWindow) {
		return;
	}

	if (currentWindow->isVisible()) {
		currentWindow->hide ();
	} else {
		currentWindow->show ();
	}
}

} /* namespace Mattermost */

int main( int argc, char *argv[])
{
	QCoreApplication::setOrganizationName("matterleast");
	QCoreApplication::setApplicationName("MatterLeast");
#ifdef MATTERLEAST_BUILD_VERSION
    QCoreApplication::setApplicationVersion(QStringLiteral(MATTERLEAST_BUILD_VERSION));
#endif
	QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::Round);

    Mattermost::MattermostApplication app(argc, argv);
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

    app.openLoginWindow();
    return app.exec();
}
