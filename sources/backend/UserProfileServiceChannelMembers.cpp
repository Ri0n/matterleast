/**
 * @file UserProfileServiceChannelMembers.cpp
 * @brief Paged channel-member primitives for virtualized member lists.
 */

#include "UserProfileService.h"

#include <algorithm>

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSet>
#include <QJsonObject>
#include <QPointer>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/QByteArrayCreator.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

void UserProfileService::queryChannelMemberCount(
    BackendChannel& channel,
    std::function<void(int)> callback)
{
    if (channel.member_count >= 0) {
        if (callback) {
            callback(channel.member_count);
        }
        return;
    }

    QPointer<BackendChannel> channelGuard(&channel);
    NetworkRequest request(
        QStringLiteral("channels/") + channel.id + QStringLiteral("/stats"));
    httpConnector.get(request, HttpResponseCallback(
        [channelGuard, callback = std::move(callback)](
            const QJsonDocument& doc) mutable {
            if (!channelGuard) {
                return;
            }
            channelGuard->member_count = std::max(0,
                doc.object().value(QStringLiteral("member_count")).toInt());
            if (callback) {
                callback(channelGuard->member_count);
            }
        }));
}

void UserProfileService::loadChannelMembersPage(
    BackendChannel& channel,
    int page,
    int perPage,
    std::function<void(QStringList)> callback)
{
    if (page < 0 || perPage <= 0) {
        if (callback) {
            callback({});
        }
        return;
    }

    QPointer<BackendChannel> channelGuard(&channel);
    NetworkRequest request(
        QStringLiteral("channels/") + channel.id
        + QStringLiteral("/members?page=") + QString::number(page)
        + QStringLiteral("&per_page=") + QString::number(perPage));

    httpConnector.get(request, HttpResponseCallback(
        [this, channelGuard, callback = std::move(callback)](
            const QJsonDocument& doc) mutable {
            if (!channelGuard) {
                if (callback) {
                    callback({});
                }
                return;
            }

            QStringList userIds;
            userIds.reserve(doc.array().size());
            for (const auto& value : doc.array()) {
                const QJsonObject memberObject = value.toObject();
                const QString userId = memberObject.value(
                    QStringLiteral("user_id")).toString();
                if (userId.isEmpty()) {
                    continue;
                }
                channelGuard->addMember(backend.getStorage(), memberObject);
                userIds.push_back(userId);
            }

            if (userIds.isEmpty()) {
                if (callback) {
                    callback({});
                }
                return;
            }

            ensureUsers(userIds,
                [this, userIds, callback = std::move(callback)]() mutable {
                    // Profiles are enough to materialize the page. Presence is
                    // independent presentation state and updates live rows via
                    // BackendUser::onStatusChanged when this batch completes.
                    if (callback) {
                        callback(userIds);
                    }
                    ensureStatuses(userIds);
                });
        }));
}

void UserProfileService::loadChannelMemberRolesByIds(
    BackendChannel& channel,
    const QStringList& userIds,
    std::function<void()> callback)
{
    QJsonArray pendingIds;
    QSet<QString> seen;
    for (const QString& userId : userIds) {
        if (userId.isEmpty() || seen.contains(userId)) {
            continue;
        }
        seen.insert(userId);
        if (!channel.members.contains(userId)) {
            pendingIds.append(userId);
        }
    }
    if (pendingIds.isEmpty()) {
        if (callback) {
            callback();
        }
        return;
    }

    QPointer<BackendChannel> channelGuard(&channel);
    QPointer<UserProfileService> guard(this);
    NetworkRequest request(QStringLiteral("channels/") + channel.id
                           + QStringLiteral("/members/ids"));
    httpConnector.post(request, QByteArrayCreator(pendingIds), HttpResponseCallback(
        [guard, channelGuard, seen, callback = std::move(callback)](
            QVariant status, const QJsonDocument& doc) mutable {
            if (guard && channelGuard
                && status.toInt() == QNetworkReply::NoError && doc.isArray()) {
                for (const QJsonValue& value : doc.array()) {
                    const QJsonObject member = value.toObject();
                    const QString userId = member.value(
                        QStringLiteral("user_id")).toString();
                    if (seen.contains(userId)
                        && member.value(QStringLiteral("channel_id")).toString()
                               == channelGuard->id) {
                        channelGuard->addMember(guard->backend.getStorage(), member);
                    }
                }
            }
            if (callback) {
                callback();
            }
        }));
}

} // namespace Mattermost
