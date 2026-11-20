#include "PluginManager.h"

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"

#include <QDateTime>

#include <algorithm>
#include <utility>

namespace Mattermost {

using MatterLeast::PluginApi::CallsState;

namespace {
constexpr auto CallsPrefix = "custom_com.mattermost.calls_";
constexpr auto CallsApiRoot = "plugins/com.mattermost.calls/";
constexpr qint64 InvitationFreshnessMs = 60000;

bool isRecentStart(qint64 startAt)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    return startAt >= now - InvitationFreshnessMs && startAt <= now + InvitationFreshnessMs;
}
}

void PluginManager::handleIncomingCallEvent(const QString& name, const QJsonObject& data,
                                            const QJsonObject& broadcast)
{
    const QString event = name.mid(int(qstrlen(CallsPrefix)));
    QString channelId = data.value(QStringLiteral("channelID")).toString();
    if (channelId.isEmpty()) {
        channelId = broadcast.value(QStringLiteral("channel_id")).toString();
    }
    if (event == QLatin1String("user_dismissed_notification")) {
        if (data.value(QStringLiteral("userID")).toString() != backend_.getLoginUser().id) {
            return;
        }
        const QString id = data.value(QStringLiteral("callID")).toString();
        for (const auto& invitation : std::as_const(incomingCalls_)) {
            if (invitation.call.id == id) {
                closeIncomingCall(invitation.call.channelId);
                break;
            }
        }
        return;
    }
    if (event == QLatin1String("call_end")) {
        endIncomingCall(channelId);
        return;
    }
    if (event != QLatin1String("call_start")) {
        return;
    }

    IncomingCall call {
        data.value(QStringLiteral("id")).toString(),
        channelId,
        data.value(QStringLiteral("post_id")).toString(),
        data.value(QStringLiteral("thread_id")).toString(),
        data.value(QStringLiteral("owner_id")).toString(),
    };
    const qint64 startAt = data.value(QStringLiteral("start_at")).toVariant().toLongLong();
    if (call.id.isEmpty() || channelId.isEmpty() || call.postId.isEmpty()
        || call.ownerId.isEmpty() || !isRecentStart(startAt)) {
        return;
    }

    const auto previous = incomingCalls_.constFind(channelId);
    if (previous != incomingCalls_.cend()
        && (previous->call.id == call.id || previous->startAt > startAt)) {
        return;
    }

    endIncomingCall(channelId);
    const auto session = callsSnapshot();
    if (call.ownerId == backend_.getLoginUser().id
        || (session.channelId == channelId && (session.state == CallsState::Connecting
                                               || session.state == CallsState::Connected))) {
        return;
    }
    incomingCalls_.insert(channelId, {std::move(call), startAt});
    announceIncomingCall(channelId);
}

void PluginManager::announceIncomingCall(const QString& channelId)
{
    auto invitation = incomingCalls_.find(channelId);
    if (invitation == incomingCalls_.end() || !invitation->pending || invitation->announced) {
        return;
    }

    if (!isRecentStart(invitation->startAt)) {
        incomingCalls_.erase(invitation);
        return;
    }
    const auto* channel = backend_.getStorage().getChannelById(channelId);
    if (!channel) {
        return; // A first DM may still be materializing from its post event.
    }
    if (channel->type != BackendChannel::directChannel
        && channel->type != BackendChannel::groupChannel) {
        incomingCalls_.erase(invitation);
        return;
    }

    invitation->announced = true;
    const auto call = invitation->call;
    emit incomingCallStarted(call);
}

QString PluginManager::incomingCallForPost(const QString& postId) const
{
    for (const auto& invitation : incomingCalls_) {
        if (invitation.pending && invitation.announced && invitation.call.postId == postId) {
            return invitation.call.id;
        }
    }
    return {};
}

void PluginManager::answerIncomingCall(const QString& callId, bool accept)
{
    if (callId.isEmpty() || !callsAvailable() || !backend_.loggedIn()
        || (accept && callsSnapshot().state != CallsState::Idle)) {
        return;
    }

    auto invitation = std::find_if(incomingCalls_.begin(), incomingCalls_.end(),
                                   [&callId](const IncomingInvitation& item) {
        return item.pending && item.announced && item.call.id == callId;
    });
    if (invitation == incomingCalls_.end() || (accept && invitation->checking)) {
        return;
    }
    const QString channelId = invitation.key();
    const auto generation = incomingCallsGeneration_;
    if (accept) {
        invitation->checking = true;
    } else {
        closeIncomingCall(channelId);
    }

    // Both server actions address the channel. Verify the invitation's ID so
    // an outdated action cannot knowingly start or dismiss a different call.
    backend_.requestCallsPlugin(QStringLiteral("GET"),
        QLatin1String(CallsApiRoot) + channelId, {}, this,
        [this, callId, channelId, accept, generation](int status, const QJsonObject& response) {
        auto current = incomingCalls_.find(channelId);
        if (generation != incomingCallsGeneration_ || current == incomingCalls_.end()
            || current->call.id != callId || current->pending != accept) {
            return;
        }
        current->checking = false;
        if (status < 200 || status >= 300) {
            return;
        }
        const auto active = response.value(QStringLiteral("call")).toObject();
        if (active.value(QStringLiteral("id")).toString() != callId) {
            endIncomingCall(channelId);
            return;
        }

        if (!accept) {
            backend_.requestCallsPlugin(QStringLiteral("POST"),
                QLatin1String(CallsApiRoot) + QStringLiteral("calls/%1/dismiss-notification").arg(channelId),
                {}, this, [](int, const QJsonObject&) {});
        } else if (callsSnapshot().state == CallsState::Idle) {
            // The provider can still be waiting for ICE configuration after
            // joinCall(). An end event for this invitation cancels that join.
            joiningIncomingCallId_ = callId;
            if (!joinCall(channelId)) {
                joiningIncomingCallId_.clear();
            }
        }
    });
}

void PluginManager::closeIncomingCall(const QString& channelId)
{
    const auto call = incomingCalls_.find(channelId);
    if (call != incomingCalls_.end() && call->pending) {
        call->pending = false;
        if (call->announced) {
            const QString id = call->call.id;
            emit incomingCallClosed(id);
        }
    }
}

void PluginManager::endIncomingCall(const QString& channelId)
{
    const auto invitation = incomingCalls_.take(channelId);
    if (invitation.call.id.isEmpty()) {
        return;
    }
    if (joiningIncomingCallId_ == invitation.call.id) {
        joiningIncomingCallId_.clear();
        leaveCall();
    }
    if (invitation.pending && invitation.announced) {
        emit incomingCallClosed(invitation.call.id);
    }
}

void PluginManager::handleIncomingCallSnapshot()
{
    const auto snapshot = callsSnapshot();
    if (!joiningIncomingCallId_.isEmpty() && !snapshot.callId.isEmpty()
        && snapshot.callId != joiningIncomingCallId_) {
        joiningIncomingCallId_.clear();
        leaveCall();
        return;
    }
    if (snapshot.state != CallsState::Connecting) {
        joiningIncomingCallId_.clear();
    }
    if (snapshot.state == CallsState::Connecting || snapshot.state == CallsState::Connected) {
        closeIncomingCall(snapshot.channelId);
    }
}

void PluginManager::clearIncomingCalls()
{
    ++incomingCallsGeneration_;
    const auto previous = std::exchange(incomingCalls_, {});
    if (!joiningIncomingCallId_.isEmpty()) {
        joiningIncomingCallId_.clear();
        leaveCall();
    }
    for (const auto& invitation : previous) {
        if (invitation.pending && invitation.announced) {
            emit incomingCallClosed(invitation.call.id);
        }
    }
}

} // namespace Mattermost
