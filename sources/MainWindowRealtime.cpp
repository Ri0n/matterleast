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

    // Interactive server dialogs remain native Qt dialogs. Their lifetime
    // will be tied to the originating session when the presenter is shared.
    auto& serverUi = ServerUiService::instance(backendInstance());
    connect(&serverUi, &ServerUiService::interactiveDialogRequested,
            this, [this](const QJsonObject& dialog,
                         const QString& url,
                         const QString& channelId,
                         const QString& teamId) {
        auto* serverDialog = new ServerDialog(
            backendInstance(), dialog, url, channelId, teamId, this);
        connect(&backendInstance(), &Backend::sessionEnding, serverDialog,
                [serverDialog] { delete serverDialog; });
        serverDialog->show();
    });


}

} // namespace Mattermost
