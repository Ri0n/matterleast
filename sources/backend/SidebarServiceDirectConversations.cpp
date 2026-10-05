/**
 * @file SidebarServiceDirectConversations.cpp
 * @brief Reconcile realtime direct/group messages with sidebar visibility preferences.
 */

#include "SidebarService.h"

#include <algorithm>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "backend/DirectConversationSidebarPolicy.h"
#include "backend/NetworkRequest.h"
#include "backend/QByteArrayCreator.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

namespace {

QJsonObject preference(const QString& userId,
                       const QString& category,
                       const QString& name,
                       const QString& value)
{
    return QJsonObject {
        {QStringLiteral("user_id"), userId},
        {QStringLiteral("category"), category},
        {QStringLiteral("name"), name},
        {QStringLiteral("value"), value},
    };
}

} // namespace

void SidebarService::resurfaceDirectConversation(BackendChannel& channel, uint64_t activityAt)
{
    if (channel.type != BackendChannel::directChannel
        && channel.type != BackendChannel::groupChannel) {
        return;
    }

    // A websocket post is newer channel activity even when the separately
    // fetched channel object still carries an older last_post_at snapshot.
    channel.last_post_at = std::max(channel.last_post_at, activityAt);

    const QString userId = currentUserId();
    if (userId.isEmpty()) {
        return;
    }

    QJsonArray preferences;
    if (channel.type == BackendChannel::directChannel) {
        const QString peerUserId = directConversationPeerUserId(userId, channel.name);
        if (peerUserId.isEmpty()) {
            return;
        }

        const bool visible = directChannelVisibility.value(peerUserId, false);

        // Mattermost stores direct_channel_show by the peer user id. The
        // existing loaded-category filter still uses the channel-name lookup,
        // so keep a local alias while persisting only the canonical key.
        directChannelVisibility.insert(peerUserId, true);
        directChannelVisibility.insert(channel.name, true);

        if (!visible) {
            const uint64_t now = static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
            activityTracker.setOpenTime(channel.id, now);
            preferences.push_back(preference(
                userId, QStringLiteral("direct_channel_show"), peerUserId,
                QStringLiteral("true")));
            preferences.push_back(preference(
                userId, QStringLiteral("channel_open_time"), channel.id,
                QString::number(now)));
        }
    } else {
        const bool visible = groupChannelVisibility.value(channel.id, false);
        groupChannelVisibility.insert(channel.id, true);
        if (!visible) {
            preferences.push_back(preference(
                userId, QStringLiteral("group_channel_show"), channel.id,
                QStringLiteral("true")));
        }
    }

    if (preferences.isEmpty()) {
        return;
    }

    NetworkRequest request(QStringLiteral("users/") + userId + QStringLiteral("/preferences"));
    httpConnector.put(request, QByteArrayCreator(preferences),
                      HttpResponseCallback([](const QJsonDocument&) {}));
}

} // namespace Mattermost
