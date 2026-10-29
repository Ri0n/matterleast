#pragma once

#include <QJsonObject>
#include <QString>

namespace Mattermost {

// Exact payload used by the official Mattermost WebSocket client for automatic
// desktop activity. Do not set manual=true here: that would override a user's
// deliberately selected Away, DND or Offline status on the server.
inline QJsonObject automaticPresenceData(bool active)
{
    return QJsonObject {
        {QStringLiteral("user_is_active"), active},
        {QStringLiteral("manual"), false},
    };
}

} // namespace Mattermost
