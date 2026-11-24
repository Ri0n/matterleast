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

#include <memory>

#include <QApplication>
#include <QSet>
#include <QTimer>

#include "build-config.h"
#if BUILD_MULTIMEDIA
#include <QSoundEffect>
#endif

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
#include "channel-tree/ChannelQuickList.h"
#include "channel-tree/ChannelTree.h"
#include "chat-area/ChatArea.h"
#include "navigation/AppNavigationService.h"
#include "notifications/NotificationManager.h"
#include "plugins/PluginManager.h"
#include "post-collection/PostCollectionView.h"
#include "server-dialog/ServerDialog.h"
#include "ui/PresenceAvatarLabel.h"
#include "ui/TeamSelectorLabel.h"

namespace Mattermost {

void MainWindow::installRealtimeUiSync()
{
    if (!backendOrNull()) return;
    static constexpr char InstalledProperty[] = "_mmqt_realtime_ui_sync_installed";
    if (property(InstalledProperty).toBool()) {
        return;
    }
    setProperty(InstalledProperty, true);

    // As in the official client, REST projections are reconciled after a
    // WebSocket reconnect, not after an arbitrary amount of wall-clock time.
    // The connector itself handles heartbeat timeouts and sequence gaps.
    connect(&backendInstance(), &Backend::onRealtimeRecovered, this, [this] {
        if (!backendInstance().loggedIn()) return;
        ui->channelList->refreshServerSnapshot();
        // The Attention sidebar has an independent CRT/followed-thread snapshot.
        FollowingModel::instance(backendInstance()).noteThreadUpdated();
        refreshSidebarViews();
    });

    // Active webapp plugins are the server-owned discovery surface for native
    // integrations too. Keep the manifest snapshot session-local; adapters can
    // subscribe to pluginsChanged() and derive installation-specific endpoints
    // from the advertised manifest without executing its JavaScript bundle.
    auto& webappPlugins = WebappPluginService::instance(backendInstance());
    webappPlugins.clear();
    webappPlugins.refresh();

    // Notification post targets use the same semantic navigation service as
    // permalinks, Following and Attention. A reply can therefore load its root
    // and exact thread position before the concrete target widget is presented.
    connect(notificationManager, &NotificationManager::activated,
            this, [this](const NotificationTarget& target) {
        if (!target.isValid() || target.accountSession != &backendInstance()) {
            return;
        }

        // The current page can be hidden behind another server in the shared
        // workspace stack. Select this Backend first, then raise only the
        // real top-level shell. Never turn a child MainWindow into a window.
        emit presentAccountRequested();
        QWidget* topLevel = window();
        if (topLevel->isMinimized()) {
            topLevel->showNormal();
        } else {
            topLevel->show();
        }
        topLevel->raise();
        topLevel->activateWindow();

        // Public/private channels have an intrinsic team. Make that team the
        // visible sidebar context before semantic navigation opens the target.
        // Direct/group conversations are server-wide and intentionally keep the
        // user's current team context.
        if (BackendChannel* channel = backendInstance().getStorage().getChannelById(target.channelId)) {
            if (channel->team) {
                TeamSelectorLabel::activateTeam(this, channel->team->id);
            }
        }

        AppNavigationService::instance(backendInstance()).openPost(target.postId);
    });

    // Standalone widget tests do not have the application's plugin manager.
    if (auto* manager = PluginManager::instance()) {
    auto& plugins = *manager;
    connect(notificationManager, &NotificationManager::incomingCallAnswered,
            this, [this, &plugins](const NotificationTarget& target, bool accept) {
        if (target.accountSession == &backendInstance())
            plugins.answerIncomingCall(plugins.accountId(backendInstance()), target.callId, accept);
    });
    auto* ringTimer = new QTimer(this);
    ringTimer->setInterval(2600);
#if BUILD_MULTIMEDIA
    auto* ringtone = new QSoundEffect(ringTimer);
    ringtone->setVolume(0.45f);
    const auto ring = [ringtone] {
        if (ringtone->source().isEmpty()) {
            ringtone->setSource(QUrl(QStringLiteral("qrc:/sounds/incoming-call.wav")));
        }
        if (ringtone->status() == QSoundEffect::Error) {
            QApplication::beep();
        } else {
            ringtone->play();
        }
    };
#else
    const auto ring = [] { QApplication::beep(); };
#endif
    connect(ringTimer, &QTimer::timeout, this, ring);
    auto ringing = std::make_shared<QSet<QString>>();
    const auto stopRinging = [ringing, ringTimer
#if BUILD_MULTIMEDIA
                              , ringtone
#endif
    ](const QString& id) {
        ringing->remove(id);
        if (ringing->isEmpty()) {
            ringTimer->stop();
#if BUILD_MULTIMEDIA
            ringtone->stop();
#endif
        }
    };
    connect(&plugins, &PluginManager::incomingCallClosed, this,
            [this, &plugins, stopRinging](const QString& accountId, const QString& id) {
        if (accountId != plugins.accountId(backendInstance())) return;
        stopRinging(id);
        notificationManager->closeIncomingCall(backendInstance(), id);
    });
    connect(&plugins, &PluginManager::incomingCallStarted, this,
            [this, &plugins, ringTimer, ring, ringing, stopRinging](const IncomingCall& call) {
        if (call.accountId != plugins.accountId(backendInstance())) return;
        auto* channel = backendInstance().getStorage().getChannelById(call.channelId);
        if (!channel || SidebarService::instance(backendInstance()).isChannelMuted(*channel)) {
            return;
        }
        const auto* owner = backendInstance().getStorage().getUserById(call.ownerId);
        const QString caller = owner ? owner->getDisplayName() : channel->display_name;
        ringing->insert(call.id);
        if (!ringTimer->isActive()) {
            ring();
            ringTimer->start();
        }
        QTimer::singleShot(30000, this, [stopRinging, id = call.id] {
            stopRinging(id);
        });
        notificationManager->show(tr("Incoming audio call"), caller,
                                  {call.channelId, call.postId,
                                   call.threadId == call.postId ? QString() : call.threadId,
                                   call.id, &backendInstance()});
    });

    }

    // Server-originated ephemeral posts are transient by definition: surface
    // them through the existing desktop notification path rather than adding
    // them to PostRepository or a channel timeline. Interactive dialogs remain
    // native Qt dialogs and never run a nested event loop.
    auto& serverUi = ServerUiService::instance(backendInstance());
    connect(&serverUi, &ServerUiService::ephemeralMessageReceived,
            this, [this](const QString& message) {
        notificationManager->show(tr("Mattermost"), message, NotificationTarget {});
    });
    connect(&serverUi, &ServerUiService::interactiveDialogRequested,
            this, [this](const QJsonObject& dialog,
                         const QString& url,
                         const QString& channelId,
                         const QString& teamId) {
        auto* serverDialog = new ServerDialog(
            backendInstance(), dialog, url, channelId, teamId, this);
        serverDialog->show();
    });

    // Alternate sidebar views can legitimately know about a direct/group
    // conversation before the server-backed Channels tree has materialized its
    // row. Route them through the Storage-aware navigation path rather than
    // requiring channelToItemMap to be populated already.
    disconnect(attentionList, &AttentionList::channelSelected,
               ui->channelList, &ChannelTree::openChannel);
    connect(attentionList, &AttentionList::channelSelected,
            ui->channelList, &ChannelTree::openStoredChannel);
    connect(attentionList, &AttentionList::conversationAttentionReactivated,
            this, [this](const QString& channelId) {
        ChatArea* active = ui->channelList->getCurrentPage();
        if (active && !active->isThread && active->getChannel().id == channelId) {
            active->flashVisibleAttention();
        }
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
