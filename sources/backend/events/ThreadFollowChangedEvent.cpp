#include "ThreadFollowChangedEvent.h"

namespace Mattermost {

ThreadFollowChangedEvent::ThreadFollowChangedEvent(const QJsonObject& data,
                                                   const QJsonObject& broadcast)
    : userId(broadcast.value(QStringLiteral("user_id")).toString())
    , teamId(broadcast.value(QStringLiteral("team_id")).toString())
    , threadId(data.value(QStringLiteral("thread_id")).toString())
    , following(data.value(QStringLiteral("state")).toBool())
    , replyCount(data.value(QStringLiteral("reply_count")).toInt())
    , valid(!threadId.isEmpty())
{
}

} // namespace Mattermost
