#include "ThreadUpdatedEvent.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace Mattermost {

ThreadUpdatedEvent::ThreadUpdatedEvent(const QJsonObject& data,
                                       const QJsonObject& broadcast)
    : userId(broadcast.value(QStringLiteral("user_id")).toString())
    , teamId(broadcast.value(QStringLiteral("team_id")).toString())
{
    const QByteArray encoded =
        data.value(QStringLiteral("thread")).toString().toUtf8();
    QJsonParseError error;
    const QJsonDocument document =
        QJsonDocument::fromJson(encoded, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        return;
    }

    threadId = document.object().value(QStringLiteral("id")).toString();
    valid = !threadId.isEmpty();
}

} // namespace Mattermost
