#pragma once

#include <QJsonObject>
#include <QString>

namespace Mattermost {

inline bool isRoutineMembershipPostType(const QString& type)
{
    return type == QStringLiteral("system_join_leave")
        || type == QStringLiteral("system_join_channel")
        || type == QStringLiteral("system_leave_channel")
        || type == QStringLiteral("system_add_remove")
        || type == QStringLiteral("system_add_to_channel")
        || type == QStringLiteral("system_remove_from_channel")
        || type == QStringLiteral("system_join_team")
        || type == QStringLiteral("system_leave_team")
        || type == QStringLiteral("system_add_to_team")
        || type == QStringLiteral("system_remove_from_team");
}

inline bool membershipPostReferencesUser(const QJsonObject& props,
                                         const QString& username)
{
    if (username.isEmpty()) {
        return false;
    }

    return props.value(QStringLiteral("username")).toString() == username
        || props.value(QStringLiteral("addedUsername")).toString() == username
        || props.value(QStringLiteral("removedUsername")).toString() == username;
}

/**
 * One semantic predicate shared by presentation and parent-channel unread state.
 * Routine membership churn is intentionally outside the main timeline, while a
 * membership event about the current user remains meaningful.
 */
inline bool participatesInMainTimeline(const QString& type,
                                       const QJsonObject& props,
                                       const QString& currentUsername)
{
    return !isRoutineMembershipPostType(type)
        || membershipPostReferencesUser(props, currentUsername);
}

inline bool shouldShowMainTimelinePost(const QString& type,
                                       const QJsonObject& props,
                                       const QString& currentUsername)
{
    return participatesInMainTimeline(type, props, currentUsername);
}

inline bool shouldCountMainTimelinePostAsUnread(const QString& type,
                                                const QJsonObject& props,
                                                const QString& currentUsername)
{
    return participatesInMainTimeline(type, props, currentUsername);
}

} // namespace Mattermost
