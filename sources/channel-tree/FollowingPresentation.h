#pragma once

#include <QString>

namespace Mattermost {

constexpr int FollowingThreadSnippetLength = 120;

inline QString compactFollowingMessage(QString message)
{
    message = message.simplified();
    if (message.size() > FollowingThreadSnippetLength) {
        message.truncate(FollowingThreadSnippetLength - 1);
        message += QChar(0x2026);
    }
    return message;
}

inline QString followingThreadLabel(const QString& channelName,
                                    const QString& message)
{
    const QString snippet = compactFollowingMessage(message);
    return snippet.isEmpty()
        ? channelName
        : channelName + QStringLiteral(" \u2014 ") + snippet;
}

} // namespace Mattermost
