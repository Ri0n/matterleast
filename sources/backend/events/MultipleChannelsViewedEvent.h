#pragma once

#include <cstdint>

#include <QHash>
#include <QJsonObject>
#include <QString>

namespace Mattermost {

class MultipleChannelsViewedEvent
{
public:
    MultipleChannelsViewedEvent(const QJsonObject& data,
                                const QJsonObject& broadcast);

    QString userId;
    QHash<QString, uint64_t> channelTimes;
};

} // namespace Mattermost
