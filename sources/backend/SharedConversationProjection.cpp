#include "SharedConversationProjection.h"

#include <QPair>
#include <QSet>

#include "Backend.h"
#include "SidebarService.h"
#include "Storage.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

QVector<SharedConversationOrder::Entry> SharedConversationProjection::collect(
    const QVector<AccountSource>& sources, const QString& sorting)
{
    QVector<SharedConversationOrder::Entry> entries;
    QSet<QPair<QString, QString>> seen;
    for (const AccountSource& source : sources) {
        if (source.accountId.isEmpty() || !source.backend)
            continue;
        auto& sidebar = SidebarService::instance(*source.backend);
        for (const QString& teamId : source.teamIds) {
            const SidebarTeamState* state = sidebar.teamState(teamId);
            if (!state)
                continue;
            for (const QString& categoryId : state->order) {
                const SidebarCategory* category = state->category(categoryId);
                if (!category)
                    continue;
                for (const QString& channelId : sidebar.visibleChannelIds(*category)) {
                    const auto identity = qMakePair(source.accountId, channelId);
                    if (seen.contains(identity))
                        continue;
                    BackendChannel* channel =
                        source.backend->getStorage().getChannelById(channelId);
                    if (!channel || (channel->type != BackendChannel::directChannel
                                     && channel->type != BackendChannel::groupChannel))
                        continue;
                    seen.insert(identity);
                    quint64 activity = static_cast<quint64>(channel->create_at);
                    activity = qMax(activity, static_cast<quint64>(channel->last_post_at));
                    activity = qMax(activity, static_cast<quint64>(
                        sidebar.channelActivityTime(*channel)));
                    entries.push_back({source.accountId, channelId,
                                       channel->display_name, activity});
                }
            }
        }
    }
    SharedConversationOrder::sort(entries, sorting);
    return entries;
}

} // namespace Mattermost
