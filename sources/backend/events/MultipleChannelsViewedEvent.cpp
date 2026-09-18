#include "MultipleChannelsViewedEvent.h"

namespace Mattermost {

MultipleChannelsViewedEvent::MultipleChannelsViewedEvent(
    const QJsonObject& data,
    const QJsonObject& broadcast)
    : userId(broadcast.value(QStringLiteral("user_id")).toString())
{
    const QJsonObject times =
        data.value(QStringLiteral("channel_times")).toObject();
    for (auto it = times.begin(); it != times.end(); ++it) {
        if (!it.key().isEmpty()) {
            channelTimes.insert(
                it.key(), it.value().toVariant().toULongLong());
        }
    }
}

} // namespace Mattermost
