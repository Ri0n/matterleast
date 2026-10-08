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

#include "login/LoginDialog.h"
#include "mainwindow.h"
#include "backend/Backend.h"
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
    QGuiApplication::setDesktopFileName(QStringLiteral("matterleast"));
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
	QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::Round);

    Mattermost::MattermostApplication app(argc, argv);

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

    app.openLoginWindow();
    return app.exec();
}
