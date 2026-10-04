#pragma once

#include <QString>

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"

namespace Mattermost::ConversationReference {

/** Mattermost's compact, composer-friendly reference for a conversation. */
inline QString copyText(Backend& backend, const BackendChannel& channel)
{
    if (channel.type == BackendChannel::directChannel) {
        if (const BackendUser* user = backend.getStorage().getUserById(channel.name);
            user && !user->username.isEmpty()) {
            return QLatin1Char('@') + user->username;
        }
        return {};
    }

    if ((channel.type == BackendChannel::publicChannel
         || channel.type == BackendChannel::privateChannel)
        && !channel.name.isEmpty()) {
        return QLatin1Char('~') + channel.name;
    }

    // Mattermost has no equivalent compact @/~ reference for a group DM.
    return {};
}

/**
 * Canonical value for Mattermost's `in:` message-search modifier.
 *
 * Keep this aligned with PostCollectionView's existing search completion:
 * named channels use their URL name; DM/GM conversations use their channel ID.
 */
inline QString searchScope(const BackendChannel& channel)
{
    const bool namedChannel = channel.type == BackendChannel::publicChannel
        || channel.type == BackendChannel::privateChannel;
    if (namedChannel && !channel.name.isEmpty()) {
        return channel.name;
    }
    return channel.id;
}

inline QString searchTerms(const BackendChannel& channel)
{
    const QString scope = searchScope(channel);
    return scope.isEmpty() ? QString()
                           : QStringLiteral("in:") + scope + QLatin1Char(' ');
}

} // namespace Mattermost::ConversationReference
