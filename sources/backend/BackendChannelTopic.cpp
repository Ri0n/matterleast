#include "Backend.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <utility>

#include "ChannelTopicPolicy.h"
#include "NetworkRequest.h"
#include "types/BackendChannel.h"

namespace Mattermost {

void Backend::editChannelHeader(BackendChannel& channel, const QString& header,
                                StatusUpdateResult callback)
{
    if (!ChannelTopicPolicy::valid(header)) {
        if (callback) {
            callback(false, tr("Channel topic must not exceed 1024 characters."));
        }
        return;
    }

    if (channel.id.isEmpty() || channel.type == BackendChannel::unknown) {
        if (callback) callback(false, tr("Invalid channel."));
        return;
    }

    if (!ChannelTopicPolicy::changed(channel.header, header)) {
        if (callback) callback(true, {});
        return;
    }

    const QString channelId = channel.id;
    NetworkRequest request(QStringLiteral("channels/") + channelId
                           + QStringLiteral("/patch"));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));

    // PATCH is a PUT endpoint in Mattermost API v4. Send ONLY 'header' to
    // avoid clobbering a concurrently edited name, purpose or other property.
    const QJsonObject patch{{QStringLiteral("header"), header}};
    QPointer<Backend> backendGuard(this);
    QPointer<BackendChannel> channelGuard(&channel);
    httpConnector.put(request, patch, HttpResponseCallback(
        [backendGuard, channelGuard, channelId, callback = std::move(callback)]
        (const QJsonDocument& response, const QNetworkReply& reply) {
        if (!backendGuard || !channelGuard) return;

        const int status = reply.attribute(
            QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool httpOk = reply.error() == QNetworkReply::NoError
            && status >= 200 && status < 300;

        if (!httpOk) {
            const QString reason = status == 403
                ? QObject::tr("You do not have permission to edit this channel topic.")
                : response.isObject()
                    ? response.object().value(QStringLiteral("message")).toString()
                    : QString();
            if (callback) {
                callback(false, !reason.isEmpty() ? reason
                    : reply.errorString().isEmpty()
                        ? QObject::tr("Could not update the channel topic.")
                        : reply.errorString());
            }
            return;
        }

        if (!response.isObject()
            || response.object().value(QStringLiteral("id")).toString()
                != channelId
            || !response.object().value(QStringLiteral("header")).isString()) {
            if (callback) {
                callback(false, QObject::tr("The server returned an invalid channel update."));
            }
            return;
        }

        // Update the canonical channel object immediately. The same update can
        // also arrive over WebSocket; emitting onUpdated keeps every open chat,
        // sidebar and topic presentation consistent without a refresh.
        channelGuard->header = response.object()
            .value(QStringLiteral("header")).toString();
        emit channelGuard->onUpdated();
        if (callback) callback(true, {});
    }));
}

} // namespace Mattermost
