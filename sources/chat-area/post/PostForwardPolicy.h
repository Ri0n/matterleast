#pragma once

#include <QString>

#include "backend/types/BackendChannel.h"

namespace Mattermost {

// Match Mattermost's forwarding privacy boundary: a permalink from a public
// channel may be shared elsewhere, while a private channel or DM/GM may only
// forward inside its original conversation.
inline bool canForwardPostTo(int sourceType,
                             const QString& sourceChannelId,
                             const QString& targetChannelId)
{
    return !sourceChannelId.isEmpty() && !targetChannelId.isEmpty()
        && (sourceType == BackendChannel::publicChannel
            || sourceChannelId == targetChannelId);
}

// Forwarding is a normal post whose last line is the original permalink.
// The server generates its own permission-gated permalink preview.
inline QString forwardedPostText(const QString& comment, const QString& permalink)
{
    if (permalink.isEmpty()) {
        return {};
    }
    return comment.trimmed().isEmpty()
        ? permalink : comment + QLatin1Char('\n') + permalink;
}

} // namespace Mattermost
