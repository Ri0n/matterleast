#pragma once

#include <QString>

namespace Mattermost {

constexpr int FollowingThreadSnippetLength = 120;

inline bool attentionMuteAllowsEntry(bool muted, bool explicitlyFollowedThread)
{
    return !muted || explicitlyFollowedThread;
}


inline QString compactFollowingMessage(QString message)
{
    message = message.simplified();
    if (message.size() > FollowingThreadSnippetLength) {
        message.truncate(FollowingThreadSnippetLength - 1);
        message += QChar(0x2026);
    }
    return message;
}

inline QString followingMessageToolTip(QString message)
{
    const auto newline = message.indexOf(QLatin1Char('\n'));
    if (newline >= 0) {
        message.truncate(newline);
    }
    message = message.trimmed();
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
