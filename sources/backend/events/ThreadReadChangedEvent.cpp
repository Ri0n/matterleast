#include "ThreadReadChangedEvent.h"

namespace Mattermost {

ThreadReadChangedEvent::ThreadReadChangedEvent(
    const QJsonObject& data,
    const QJsonObject& broadcast)
    : userId(broadcast.value(QStringLiteral("user_id")).toString())
    , teamId(broadcast.value(QStringLiteral("team_id")).toString())
    , channelId(data.value(QStringLiteral("channel_id")).toString())
    , threadId(data.value(QStringLiteral("thread_id")).toString())
    , timestamp(data.value(QStringLiteral("timestamp")).toVariant().toULongLong())
    , previousUnreadMentions(
          data.value(QStringLiteral("previous_unread_mentions")).toInt())
    , previousUnreadReplies(
          data.value(QStringLiteral("previous_unread_replies")).toInt())
    , unreadMentions(data.value(QStringLiteral("unread_mentions")).toInt())
    , unreadReplies(data.value(QStringLiteral("unread_replies")).toInt())
{
    if (channelId.isEmpty()) {
        channelId = broadcast.value(QStringLiteral("channel_id")).toString();
    }

    // Mattermost uses one event type for three scopes:
    // one thread, every thread in a channel, or every thread in a team.
    valid = !threadId.isEmpty() || !channelId.isEmpty() || !teamId.isEmpty();
}

} // namespace Mattermost
