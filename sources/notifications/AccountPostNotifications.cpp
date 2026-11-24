#include "AccountPostNotifications.h"

#include <utility>
#include <QDateTime>
#include <QPointer>
#include <QWidget>

#include "NotificationManager.h"
#include "RootNotificationPolicy.h"
#include "DesktopPostFreshnessPolicy.h"
#include "Settings.h"
#include "backend/Backend.h"
#include "backend/FollowingModel.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendUser.h"
#include "chat-area/PostFilterPolicy.h"
#include "options/MLOptions.h"
#include "plugins/PluginManager.h"

namespace Mattermost {

AccountPostNotifications::AccountPostNotifications(NotificationManager& notifications,
                                                   Visibility visible, Window window,
                                                   QObject* parent)
    : QObject(parent), notifications_(notifications), visible_(std::move(visible)),
      window_(std::move(window))
{
}

AccountPostNotifications::~AccountPostNotifications()
{
    // Disconnect callbacks before the monitor's own members disappear.
    for (auto* context : subscriptions_)
        delete context;
}

void AccountPostNotifications::watch(Backend& source)
{
    if (subscriptions_.contains(&source)) return;
    auto* context = new QObject(this);
    subscriptions_.insert(&source, context);
    const QPointer<Backend> guard(&source);
    connect(&source, &Backend::onNewPost, context,
            [this, guard](BackendChannel& channel, const BackendPost& post) {
        if (guard) notify(*guard, channel, post);
    });
    connect(&source, &QObject::destroyed, context, [this, key = &source] {
        unwatch(key);
    });
}

void AccountPostNotifications::unwatch(const Backend* source)
{
    delete subscriptions_.take(source);
}

void AccountPostNotifications::notify(Backend& source, BackendChannel& channel,
                                      const BackendPost& post)
{
    if (post.type == QLatin1String("custom_calls")
        && (channel.type == BackendChannel::directChannel
            || channel.type == BackendChannel::groupChannel)
        && PluginManager::instance()
        && PluginManager::instance()->callsAvailable()) {
        return;  // Calls owns the live invitation notification.
    }
    if (post.isOwnPost()) {
        return;
    }

    const BackendUser* loginUser = source.getStorage().loginUser;
    if (!shouldShowMainTimelinePost(post.type, post.props.toObject(),
                                    loginUser ? loginUser->username : QString())) {
        // Also keep suppressed membership events out of the explicit mention
        // path, not just the desktop notification path.
        return;
    }

    auto& sidebar = SidebarService::instance(source);
    // SidebarService::recordChannelPost() already tracks reply mentions in
    // the thread domain. Never promote a reply mention to a root-channel
    // mention: with CRT enabled this would pin the parent DM in Attention
    // even after the thread's read acknowledgement succeeds.
    // WebSocket recovery may replay historical posted events. Keep the
    // notification-only mention side effect out of stale replay too; actual
    // unread/Attention reconciliation is handled independently in the backend.
    if (!isFreshDesktopPost(post.create_at,
                            static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()))) {
        return;
    }

    // Membership and CRT snapshots expose different read watermarks. Do not
    // use the optimistic local read cursor (or the channel cursor for CRT
    // replies): only server-confirmed progress suppresses a desktop popup.
    const FollowingModel::Entry* threadState = post.root_id.isEmpty()
        ? nullptr : FollowingModel::instance(source).findEntry(channel.id, post.root_id);
    const quint64 threadServerReadAt = threadState && threadState->isThread()
        && !threadState->synthetic ? threadState->lastViewedAt : 0;
    if (!isUnreadDesktopPost(
            post.create_at, !post.root_id.isEmpty(),
            sidebar.usesRootUnreadCounts(channel),
            sidebar.channelLastServerViewedTime(channel.id),
            threadServerReadAt)) {
        return;
    }
    if (post.currentUserMentioned && post.root_id.isEmpty()) {
        sidebar.setChannelMentioned(channel.id, true);
    }
    if (sidebar.isChannelMuted(channel)) {
        return;
    }

    const bool directConversation = channel.type == BackendChannel::directChannel
        || channel.type == BackendChannel::groupChannel;
    if (post.root_id.isEmpty()) {
        // The user preference controls only public/private channel ROOT
        // messages. Muting and active-channel suppression remain outside
        // this policy. Direct/group messages keep their existing behavior.
        const auto mode = channelRootNotificationModeFromSetting(
            MLOptions::instance()->value<int>(
                CHANNEL_ROOT_NOTIFICATION_MODE,
                CHANNEL_ROOT_NOTIFICATION_MODE_DEFAULT));
        if (!shouldNotifyRootPost(
                directConversation, post.currentUserMentioned,
                sidebar.isChannelFavorite(channel), mode)) {
            return;
        }
    } else {
        if (!shouldNotifyThreadReply(post.currentUserMentioned,
                threadState && threadState->isThread() && !threadState->synthetic)) {
            return;
        }
    }

    // A parent channel, another central tab and a docked/detached thread are
    // independent reading surfaces. Suppress only when this exact destination
    // is visibly presented in its focused window, not merely because a parent
    // channel is selected somewhere in the sidebar.
    if (visible_ && visible_(source, channel.id, post.root_id)) {
        return;
    }

    QString title;
    if (!post.root_id.isEmpty()) {
        title = post.getDisplayAuthorName()
            + (post.currentUserMentioned
                ? " mentioned you in a thread"
                : " replied in a followed thread");
    } else if (channel.type == BackendChannel::directChannel) {
        title = post.getDisplayAuthorName() + " messaged you";
    } else {
        title = post.getDisplayAuthorName() + " posted in '" + channel.display_name + "'";
    }

    notifications_.show(title, post.message,
                              NotificationTarget {channel.id, post.id, post.root_id, {}, &source});
    // Taskbar urgency belongs to the actual application window, not to a
    // hidden account's presentation.
    if (auto* window = window_ ? window_() : nullptr)
        notifications_.requestTaskbarAttention(*window);
}


} // namespace Mattermost
