#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace Mattermost::CallsMessagePack {

// The Calls server requires websocket SDP as zlib-compressed JSON inside a
// MessagePack binary frame (not as an ordinary JSON websocket message).
// Limit this encoder to the exact supported Calls message shape.
QByteArray encodeSdp(int sequence, const QJsonObject& description);

} // namespace Mattermost::CallsMessagePack
