/**
 * @file MainWindowRealtime.cpp
 * @brief Cross-view synchronization for realtime conversation state.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "mainwindow.h"

#include "ui_mainwindow.h"


#include "backend/Backend.h"
#include "backend/FollowingModel.h"
#include "backend/ServerUiService.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/WebappPluginService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendDirectChannelsTeam.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/AttentionList.h"
#include "channel-tree/ChannelTree.h"
#include "post-collection/PostCollectionView.h"
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
        serverDialog->show();
    });

    auto connectConversationSource = [this](BackendDirectChannelsTeam& conversations) {
        connect(&conversations, &BackendDirectChannelsTeam::onNewChannel,
                this, [this](BackendChannel& channel) {
            // direct_added/channel fetch updates Storage first. Admit that
            // authoritative object into the already loaded Direct Messages
            // categories immediately; no category HTTP round-trip is needed.
            ui->channelList->admitStoredConversation(channel);
            refreshSidebarViews();
        });
    };
    connectConversationSource(backendInstance().getStorage().directChannels);
    connectConversationSource(backendInstance().getStorage().groupChannels);

    // Mattermost's webapp runs loadNewDMIfNeeded/loadNewGMIfNeeded for every
    // posted websocket event. An old conversation may no longer be present in
    // the limited Direct Messages category, so restore its preference state and
    // locally admit it immediately instead of leaving it visible only in
    // Attention until the next full sidebar refresh.
    connect(&backendInstance(), &Backend::onNewPost,
            this, [this](BackendChannel& channel, const BackendPost& post) {
        if (channel.type != BackendChannel::directChannel
            && channel.type != BackendChannel::groupChannel) {
            return;
        }

        SidebarService::instance(backendInstance()).resurfaceDirectConversation(
            channel, post.create_at);
        ui->channelList->admitStoredConversation(channel);
        refreshSidebarViews();
    });

    // Mattermost synchronizes flagged_post through preference websocket
    // events. Feed them into the collection only when it has been opened; its
    // initial activation already loads the authoritative current snapshot.
    connect(&backendInstance(), &Backend::onFlaggedPostChanged,
            this, [this](const QString& postId, bool flagged) {
        if (savedMessagesPage) {
            savedMessagesPage->syncFlaggedPost(postId, flagged);
        }
    });
}

} // namespace Mattermost
