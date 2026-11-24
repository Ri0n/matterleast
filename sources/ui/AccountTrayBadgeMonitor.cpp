#include "AccountTrayBadgeMonitor.h"

#include <QPointer>
#include <QSystemTrayIcon>

#include "backend/Backend.h"
#include "backend/FollowingModel.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/FollowingPresentation.h"
#include "ui/IconUtils.h"
#include "ui/TrayBadgePolicy.h"

namespace Mattermost {

AccountTrayBadgeMonitor::AccountTrayBadgeMonitor(QSystemTrayIcon& tray)
    : QObject(&tray), tray_(tray)
{
}

void AccountTrayBadgeMonitor::watch(Backend& owner)
{
    if (subscriptions_.contains(&owner)) return;
    auto* context = new QObject(this);
    subscriptions_.insert(&owner, context);
    QPointer<Backend> guard(&owner);
    auto changed = [this, guard] {
        if (guard) refresh(*guard);
    };
    auto& following = FollowingModel::instance(owner);
    connect(&following, &FollowingModel::changed, context, changed);
    connect(&SidebarService::instance(owner), &SidebarService::categoriesChanged,
            context, changed);
    connect(&owner, &QObject::destroyed, context, [this, key = &owner] {
        unwatch(key);
    });
    following.ensureThreadsFresh();
    refresh(owner);
}

void AccountTrayBadgeMonitor::unwatch(const Backend* owner)
{
    delete subscriptions_.take(owner);
    badges_.remove(owner);
    updateIcon();
}

void AccountTrayBadgeMonitor::refresh(Backend& owner)
{
    if (!subscriptions_.contains(&owner)) return;
    std::uint32_t count = 0;
    for (const auto& entry : FollowingModel::instance(owner).entries()) {
        if (entry.requiresAttention()
            && attentionMuteAllowsEntry(entry.muted, entry.isThread() && !entry.synthetic))
            ++count;
    }

    bool unreadChannel = false;
    if (count == 0) {
        auto& sidebar = SidebarService::instance(owner);
        for (const auto* channel : owner.getStorage().channels) {
            if (!channel || channel->delete_at != 0) continue;
            const bool publicOrPrivate = channel->type == BackendChannel::publicChannel
                || channel->type == BackendChannel::privateChannel;
            if (!publicOrPrivate || sidebar.isChannelMuted(*channel)
                || !sidebar.isChannelUnread(*channel))
                continue;
            if (isTrayUnreadChannel(publicOrPrivate,
                                    sidebar.isChannelTracked(channel->id),
                                    false, true)) {
                unreadChannel = true;
                break;
            }
        }
    }

    badges_.update(&owner, count, unreadChannel);
    updateIcon();
}

void AccountTrayBadgeMonitor::updateIcon()
{
    const auto next = aggregate();
    if (next.attentionCount == displayed_.attentionCount
        && next.hasUnreadChannel == displayed_.hasUnreadChannel)
        return;
    displayed_ = next;
    tray_.setIcon(IconUtils::applicationIcon(next.attentionCount, next.hasUnreadChannel));
}

} // namespace Mattermost
