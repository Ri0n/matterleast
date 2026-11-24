#include "AccountNotifications.h"

#include <utility>
#include <QApplication>
#include <QDateTime>
#include <QUrl>
#include "build-config.h"
#if BUILD_MULTIMEDIA
#include <QSoundEffect>
#endif
#include <QPointer>
#include <QWidget>

#include "NotificationManager.h"
#include "RootNotificationPolicy.h"
#include "DesktopPostFreshnessPolicy.h"
#include "Settings.h"
#include "backend/Backend.h"
#include "backend/FollowingModel.h"
#include "backend/SidebarService.h"
#include "backend/ServerUiService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendUser.h"
#include "chat-area/PostFilterPolicy.h"
#include "options/MLOptions.h"
#include "plugins/PluginManager.h"

namespace Mattermost {

AccountNotifications::AccountNotifications(NotificationManager& notifications,
                                                   PluginManager& plugins,
                                                   Visibility visible, Window window,
                                                   QObject* parent)
    : QObject(parent), notifications_(notifications), plugins_(plugins),
      visible_(std::move(visible)), window_(std::move(window))
{
    ringTimer_.setInterval(2600);
    connect(&ringTimer_, &QTimer::timeout, this, &AccountNotifications::playRingtone);
    connect(&plugins_, &PluginManager::incomingCallStarted,
            this, &AccountNotifications::incomingCall);
    connect(&plugins_, &PluginManager::incomingCallClosed, this,
            [this](const QString& accountId, const QString& callId) {
        if (auto* source = sourceForAccount(accountId)) {
            stopRinging(source, callId);
            notifications_.closeIncomingCall(*source, callId);
        }
    });
    connect(&notifications_, &NotificationManager::incomingCallAnswered, this,
            [this](const NotificationTarget& target, bool accept) {
        auto* source = qobject_cast<Backend*>(target.accountSession.data());
        if (!source || !subscriptions_.contains(source)) return;
        plugins_.answerIncomingCall(plugins_.accountId(*source), target.callId, accept);
    });
}

AccountNotifications::~AccountNotifications()
{
    // Disconnect callbacks before the monitor's own members disappear.
    for (const auto& subscription : subscriptions_)
        delete subscription.context;
}

void AccountNotifications::watch(Backend& source)
{
    if (subscriptions_.contains(&source)) return;
    auto* context = new QObject(this);
    subscriptions_.insert(&source, {&source, context});
    const QPointer<Backend> guard(&source);
    connect(&source, &Backend::onNewPost, context,
            [this, guard](BackendChannel& channel, const BackendPost& post) {
        if (guard) notify(*guard, channel, post);
    });
    connect(&source, &QObject::destroyed, context, [this, key = &source] {
        unwatch(key);
    });
    connect(&ServerUiService::instance(source), &ServerUiService::ephemeralMessageReceived,
            context, [this](const QString& message) {
        notifications_.show(tr("Mattermost"), message, NotificationTarget{});
    });
}

void AccountNotifications::unwatch(const Backend* source)
{
    const auto subscription = subscriptions_.take(source);
    delete subscription.context;
    if (subscription.source)
        notifications_.clearAccountSession(*subscription.source);
    ringing_.remove(source);
    stopRingtoneIfIdle();
}

Backend* AccountNotifications::sourceForAccount(const QString& accountId) const
{
    if (accountId.isEmpty()) return nullptr;
    for (const auto& subscription : subscriptions_) {
        if (subscription.source && plugins_.accountId(*subscription.source) == accountId)
            return subscription.source.data();
    }
    return nullptr;
}

void AccountNotifications::incomingCall(const IncomingCall& call)
{
    Backend* source = sourceForAccount(call.accountId);
    if (!source || call.id.isEmpty()) return;
    const auto* channel = source->getStorage().getChannelById(call.channelId);
    if (!channel || SidebarService::instance(*source).isChannelMuted(*channel)) return;
    const auto* caller = source->getStorage().getUserById(call.ownerId);
    const QString callerName = caller ? caller->getDisplayName() : channel->display_name;
    // Account + call ID is the key, not a server-wide call ID alone. A late
    // timeout for a previous invitation cannot stop a replacement.
    const quint64 generation = ++callGeneration_;
    ringing_[source].insert(call.id, generation);
    if (!ringTimer_.isActive()) {
        playRingtone();
        ringTimer_.start();
    }
    QPointer<Backend> guard(source);
    QTimer::singleShot(30000, this, [this, guard, id = call.id, generation] {
        if (guard) stopRinging(guard.data(), id, generation);
    });
    notifications_.show(tr("Incoming audio call"), callerName,
                        {call.channelId, call.postId,
                         call.threadId == call.postId ? QString() : call.threadId,
                         call.id, source});
}

void AccountNotifications::stopRinging(const Backend* source, const QString& callId,
                                            quint64 generation)
{
    auto account = ringing_.find(source);
    if (account == ringing_.end()) return;
    auto invitation = account->find(callId);
    if (invitation == account->end() || (generation && invitation.value() != generation))
        return;
    account->erase(invitation);
    if (account->isEmpty())
        ringing_.erase(account);
    stopRingtoneIfIdle();
}

void AccountNotifications::playRingtone()
{
#if BUILD_MULTIMEDIA
    if (!ringtone_) {
        ringtone_ = new QSoundEffect(this);
        ringtone_->setVolume(0.45f);
        ringtone_->setSource(QUrl(QStringLiteral("qrc:/sounds/incoming-call.wav")));
    }
    if (ringtone_->status() != QSoundEffect::Error) {
        ringtone_->play();
        return;
    }
#endif
    QApplication::beep();
}

void AccountNotifications::stopRingtoneIfIdle()
{
    if (!ringing_.isEmpty()) return;
    ringTimer_.stop();
#if BUILD_MULTIMEDIA
    if (ringtone_) ringtone_->stop();
#endif
}

void AccountNotifications::notify(Backend& source, BackendChannel& channel,
                                      const BackendPost& post)
{
    if (post.type == QLatin1String("custom_calls")
        && (channel.type == BackendChannel::directChannel
            || channel.type == BackendChannel::groupChannel)
        && plugins_.callsAvailable()) {
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
