#include "SharedConversationProjection.h"

#include <algorithm>

#include "Backend.h"
#include "SidebarService.h"
#include "Storage.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

QVector<SharedConversationOrder::Entry> SharedConversationProjection::collect(
    const QVector<AccountSource>& sources, const QString& sorting)
{
    QVector<SharedConversationOrder::Entry> entries;
    for (const AccountSource& source : sources) {
        if (source.accountId.isEmpty() || !source.backend)
            continue;
        auto& storage = source.backend->getStorage();
        auto& sidebar = SidebarService::instance(*source.backend);
        for (const QString& channelId : sidebar.visibleDirectConversationIds()) {
            const BackendChannel* channel = storage.getChannelById(channelId);
            if (!channel)
                continue;
            quint64 activity = std::max<quint64>(channel->create_at,
                                                  channel->last_post_at);
            activity = std::max(activity, static_cast<quint64>(
                sidebar.channelActivityTime(*channel)));
            entries.push_back({source.accountId, channelId,
                               channel->display_name, activity});
        }
    }
    SharedConversationOrder::sort(entries, sorting);
    return entries;
}

} // namespace Mattermost
