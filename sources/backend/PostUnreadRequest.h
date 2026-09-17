#pragma once

#include <QJsonObject>
#include <QString>

namespace Mattermost {

inline QString postUnreadPath(const QString& userId, const QString& postId)
{
    return QStringLiteral("users/") + userId + QStringLiteral("/posts/") + postId
        + QStringLiteral("/set_unread");
}

inline QJsonObject postUnreadPayload(bool collapsedThreadsSupported)
{
    return QJsonObject {{QStringLiteral("collapsed_threads_supported"),
                         collapsedThreadsSupported}};
}

inline QString threadUnreadPath(const QString& userId,
                                const QString& teamId,
                                const QString& threadId,
                                const QString& postId)
{
    return QStringLiteral("users/") + userId
        + QStringLiteral("/teams/") + teamId
        + QStringLiteral("/threads/") + threadId
        + QStringLiteral("/set_unread/") + postId;
}

} // namespace Mattermost
