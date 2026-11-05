/**
 * @file WebSocketEventHandlerEphemeral.cpp
 * @brief Handling of transient server-side websocket messages.
 */

#include "WebSocketEventHandler.h"

#include <QJsonDocument>

#include "Backend.h"
#include "ServerUiService.h"

namespace Mattermost {

void WebSocketEventHandler::handleEphemeralMessage(const QJsonObject& data)
{
    const QJsonObject post = QJsonDocument::fromJson(
        data.value(QStringLiteral("post")).toString().toUtf8()).object();
    if (post.isEmpty()) {
        return;
    }

    // Mattermost sends out-of-channel mention guidance in structured props.
    // It is not a durable post; do not add it to PostRepository or Channel.
    const QJsonObject addMember = post.value(QStringLiteral("props")).toObject()
        .value(QStringLiteral("add_channel_member")).toObject();
    if (post.value(QStringLiteral("type")).toString()
            == QStringLiteral("system_ephemeral")
        && !addMember.isEmpty()) {
        emit backend.onEphemeralPost(post);
        return;
    }

    const QString message = post.value(QStringLiteral("message")).toString();
    if (!message.isEmpty()) {
        ServerUiService::instance(backend).notifyEphemeralMessage(message);
    }
}

} // namespace Mattermost
