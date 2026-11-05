#include "Backend.h"

#include <memory>
#include <utility>

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>

#include "NetworkRequest.h"
#include "QByteArrayCreator.h"
#include "types/BackendChannel.h"
#include "types/BackendTeam.h"

namespace Mattermost {
namespace {

bool teamChannel(const BackendChannel& channel)
{
    return channel.delete_at == 0 && channel.team
        && (channel.type == BackendChannel::publicChannel
            || channel.type == BackendChannel::privateChannel);
}

} // namespace

void Backend::hasChannelPermissions(BackendChannel& channel,
                                    const QStringList& required,
                                    std::function<void(bool)> callback)
{
    if (!teamChannel(channel)) {
        callback(false);
        return;
    }

    struct RoleLookup {
        QStringList names;
        int pending = 2;
    };
    auto state = std::make_shared<RoleLookup>();
    state->names = getLoginUser().roles.join(QLatin1Char(' ')).split(
        QLatin1Char(' '), Qt::SkipEmptyParts);
    const QString userId = getLoginUser().id;
    const QString host = NetworkRequest::host();
    QPointer<Backend> guard(this);
    QPointer<BackendChannel> channelGuard(&channel);
    const auto membershipLoaded =
        [guard, channelGuard, state, userId, host, required, callback]
        (QVariant status, const QJsonDocument& doc) {
        if (!guard || !channelGuard) return;
        if (status.toInt() == QNetworkReply::NoError && doc.isObject()
            && doc.object().value(QStringLiteral("user_id")).toString() == userId
            && doc.object().value(QStringLiteral("delete_at")).toDouble() == 0) {
            state->names.append(doc.object().value(QStringLiteral("roles")).toString()
                                    .split(QLatin1Char(' '), Qt::SkipEmptyParts));
        }
        if (--state->pending != 0) return;
        if (guard->getLoginUser().id != userId || NetworkRequest::host() != host) {
            callback(false);
            return;
        }
        state->names.removeDuplicates();
        if (state->names.isEmpty()) {
            callback(false);
            return;
        }
        // Membership responses contain the effective scheme/custom role names.
        // Resolve their current server permissions instead of assuming that an
        // admin flag (or a built-in role name) grants membership management.
        QJsonArray names;
        for (const QString& name : std::as_const(state->names)) names.append(name);
        NetworkRequest request(QStringLiteral("roles/names"));
        guard->httpConnector.post(request, names, HttpResponseCallback(
            [guard, channelGuard, userId, host, required, callback]
            (QVariant roleStatus, const QJsonDocument& roles) {
            if (!guard || !channelGuard) return;
            QSet<QString> granted;
            if (guard->getLoginUser().id == userId && NetworkRequest::host() == host
                && teamChannel(*channelGuard)
                && roleStatus.toInt() == QNetworkReply::NoError && roles.isArray()) {
                for (const QJsonValue& value : roles.array()) {
                    const auto role = value.toObject();
                    if (role.value(QStringLiteral("delete_at")).toDouble() != 0) {
                        continue;
                    }
                    for (const QJsonValue& permission :
                         role.value(QStringLiteral("permissions")).toArray()) {
                        granted.insert(permission.toString());
                    }
                }
            }
            bool allowed = true;
            for (const QString& permission : required) {
                if (!granted.contains(permission)) {
                    allowed = false;
                    break;
                }
            }
            callback(allowed);
        }));
    };
    NetworkRequest channelRequest(QStringLiteral("channels/") + channel.id
        + QStringLiteral("/members/") + userId);
    httpConnector.get(channelRequest, HttpResponseCallback(membershipLoaded));
    NetworkRequest teamRequest(QStringLiteral("teams/") + channel.team->id
        + QStringLiteral("/members/") + userId);
    httpConnector.get(teamRequest, HttpResponseCallback(membershipLoaded));
}

void Backend::canAddChannelMembers(BackendChannel& channel,
                                   std::function<void(bool)> callback)
{
    const QString membersPermission = channel.type == BackendChannel::publicChannel
        ? QStringLiteral("manage_public_channel_members")
        : QStringLiteral("manage_private_channel_members");
    hasChannelPermissions(channel, {membersPermission}, std::move(callback));
}

void Backend::canManageChannelRoles(BackendChannel& channel,
                                    std::function<void(bool)> callback)
{
    const QString membersPermission = channel.type == BackendChannel::publicChannel
        ? QStringLiteral("manage_public_channel_members")
        : QStringLiteral("manage_private_channel_members");
    hasChannelPermissions(channel,
                          {membersPermission, QStringLiteral("manage_channel_roles")},
                          std::move(callback));
}

void Backend::setChannelMemberAdmin(BackendChannel& channel,
                                    const QString& userId,
                                    bool admin,
                                    StatusUpdateResult callback)
{
    if (!teamChannel(channel) || userId.isEmpty()
        || !channel.members.contains(userId)) {
        if (callback) {
            callback(false, tr("Invalid channel member."));
        }
        return;
    }

    const QString channelId = channel.id;
    const QString currentUserId = getLoginUser().id;
    const QString host = NetworkRequest::host();
    QPointer<Backend> backendGuard(this);
    QPointer<BackendChannel> channelGuard(&channel);
    const auto fail = [callback](const QJsonDocument& doc, const QNetworkReply& reply) {
        const QString message = doc.object().value(QStringLiteral("message")).toString();
        if (callback) {
            callback(false, !message.isEmpty() ? message
                : reply.errorString().isEmpty()
                    ? QObject::tr("Could not change the channel member's role.")
                    : reply.errorString());
        }
    };

    NetworkRequest request(QStringLiteral("channels/") + channelId
                           + QStringLiteral("/members/") + userId
                           + QStringLiteral("/schemeRoles"));
    const QJsonObject body {
        {QStringLiteral("scheme_user"), true},
        {QStringLiteral("scheme_admin"), admin}
    };
    httpConnector.put(request, body, HttpResponseCallback(
        [backendGuard, channelGuard, channelId, userId, admin, currentUserId,
         host, callback = std::move(callback), fail]
        (const QJsonDocument& response, const QNetworkReply& reply) {
        if (!backendGuard || !channelGuard
            || backendGuard->getLoginUser().id != currentUserId
            || NetworkRequest::host() != host) {
            return;
        }
        const int code = reply.attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply.error() != QNetworkReply::NoError || code < 200 || code >= 300) {
            fail(response, reply);
            return;
        }

        // PUT returns only {"status":"OK"}, not the updated member.
        // Read it back before updating the canonical role and the row.
        NetworkRequest getMember(QStringLiteral("channels/") + channelId
                                 + QStringLiteral("/members/") + userId);
        backendGuard->httpConnector.get(getMember, HttpResponseCallback(
            [backendGuard, channelGuard, channelId, userId, admin, currentUserId,
             host, callback](const QJsonDocument& memberDoc,
                             const QNetworkReply& memberReply) {
            if (!backendGuard || !channelGuard
                || backendGuard->getLoginUser().id != currentUserId
                || NetworkRequest::host() != host) {
                return;
            }
            const QJsonObject member = memberDoc.object();
            if (memberReply.error() != QNetworkReply::NoError
                || member.value(QStringLiteral("channel_id")).toString() != channelId
                || member.value(QStringLiteral("user_id")).toString() != userId
                || member.value(QStringLiteral("scheme_admin")).toBool() != admin) {
                if (callback) {
                    callback(false, QObject::tr(
                        "The server did not confirm the updated channel role."));
                }
                return;
            }
            channelGuard->addMember(backendGuard->getStorage(), member);
            if (callback) {
                callback(true, {});
            }
        }));
    }));
}

void Backend::addChannelMember(BackendChannel& channel, const QString& userId,
                               StatusUpdateResult callback)
{
    if (!teamChannel(channel) || userId.isEmpty()) {
        callback(false, tr("Invalid channel member."));
        return;
    }
    NetworkRequest request(QStringLiteral("channels/") + channel.id + QStringLiteral("/members"));
    const QJsonObject payload {{QStringLiteral("user_id"), userId}};
    const QString channelId = channel.id;
    QPointer<Backend> guard(this);
    QPointer<BackendChannel> channelGuard(&channel);
    httpConnector.post(request, payload, HttpResponseCallback(
        [guard, channelGuard, channelId, userId, callback = std::move(callback)]
        (const QJsonDocument& doc, const QNetworkReply& reply) {
        if (!guard || !channelGuard) return;
        const QJsonObject member = doc.object();
        if (reply.error() != QNetworkReply::NoError) {
            const QString message = member.value(QStringLiteral("message")).toString();
            callback(false, message.isEmpty() ? reply.errorString() : message);
            return;
        }
        if (member.value(QStringLiteral("channel_id")).toString() != channelId
            || member.value(QStringLiteral("user_id")).toString() != userId) {
            callback(false, QObject::tr("The server returned an invalid channel member."));
            return;
        }
        channelGuard->addMember(guard->getStorage(), member);
        if (const BackendUser* user = guard->getStorage().getUserById(userId)) {
            emit channelGuard->onUserAdded(*user);
        }
        callback(true, {});
    }));
}

} // namespace Mattermost
