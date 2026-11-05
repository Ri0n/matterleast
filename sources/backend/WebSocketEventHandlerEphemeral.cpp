/**
 * @file WebSocketEventHandlerEphemeral.cpp
 * @brief Forward transient websocket messages to Backend for dispatch.
 */

#include "WebSocketEventHandler.h"

#include "Backend.h"

namespace Mattermost {

void WebSocketEventHandler::handleEphemeralMessage(const QJsonObject& data)
{
    // Transport adapters do not inspect post type, props or display content.
    backend.handleEphemeralMessage(data);
}

} // namespace Mattermost
