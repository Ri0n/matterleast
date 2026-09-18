#pragma once

#include <QJsonObject>
#include <QString>

namespace Mattermost {

class ThreadUpdatedEvent
{
public:
    ThreadUpdatedEvent(const QJsonObject& data,
                       const QJsonObject& broadcast);

    QString userId;
    QString teamId;
    QString threadId;
    bool valid = false;
};

} // namespace Mattermost
