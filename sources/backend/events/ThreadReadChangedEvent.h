#pragma once

#include <cstdint>

#include <QJsonObject>
#include <QString>

namespace Mattermost {

class ThreadReadChangedEvent
{
public:
    ThreadReadChangedEvent(const QJsonObject& data,
                           const QJsonObject& broadcast);

    QString userId;
    QString teamId;
    QString channelId;
    QString threadId;
    uint64_t timestamp = 0;
    int previousUnreadMentions = 0;
    int previousUnreadReplies = 0;
    int unreadMentions = 0;
    int unreadReplies = 0;
    bool valid = false;
};

} // namespace Mattermost
