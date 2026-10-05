#pragma once

#include <QString>
#include <QStringList>

namespace Mattermost {

inline QString directConversationPeerUserId(const QString& currentUserId,
                                            const QString& channelName)
{
    const QStringList userIds = channelName.split(QStringLiteral("__"));
    if (userIds.size() != 2) {
        return {};
    }
    if (userIds.at(0) == currentUserId) {
        return userIds.at(1);
    }
    if (userIds.at(1) == currentUserId) {
        return userIds.at(0);
    }
    return {};
}

inline bool promoteSidebarConversation(QStringList& channelIds, const QString& channelId)
{
    if (channelId.isEmpty()) {
        return false;
    }
    if (!channelIds.isEmpty() && channelIds.first() == channelId
        && channelIds.count(channelId) == 1) {
        return false;
    }

    channelIds.removeAll(channelId);
    channelIds.prepend(channelId);
    return true;
}

} // namespace Mattermost
