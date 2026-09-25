#pragma once

#include <QJsonObject>
#include <QString>

namespace Mattermost {

class ThreadFollowChangedEvent
{
public:
    ThreadFollowChangedEvent(const QJsonObject& data,
                             const QJsonObject& broadcast);

    QString userId;
    QString teamId;
    QString threadId;
    bool following = false;
    int replyCount = 0;
    bool valid = false;
};

} // namespace Mattermost
