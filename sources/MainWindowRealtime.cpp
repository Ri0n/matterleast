/**
 * @file MainWindowRealtime.cpp
 * @brief Cross-view synchronization for realtime conversation state.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "mainwindow.h"

#include "backend/Backend.h"
#include "backend/ServerUiService.h"
#include "server-dialog/ServerDialog.h"

namespace Mattermost {

void MainWindow::installRealtimeUiSync()
{
    if (!backendOrNull()) return;
    static constexpr char InstalledProperty[] = "_mmqt_realtime_ui_sync_installed";
    if (property(InstalledProperty).toBool()) {
        return;
    }
    setProperty(InstalledProperty, true);

    installRealtimeUiSync(backendInstance(), this);
}

void MainWindow::installRealtimeUiSync(Backend& source, QObject* context)
{
    auto& serverUi = ServerUiService::instance(source);
    connect(&serverUi, &ServerUiService::interactiveDialogRequested,
            context, [this, &source](const QJsonObject& dialog,
                         const QString& url,
                         const QString& channelId,
                         const QString& teamId) {
        auto* serverDialog = new ServerDialog(
            source, dialog, url, channelId, teamId, this);
        connect(&source, &Backend::sessionEnding, serverDialog,
                [serverDialog] { delete serverDialog; });
        serverDialog->show();
    });


}

} // namespace Mattermost
