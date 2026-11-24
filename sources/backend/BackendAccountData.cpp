#include "Backend.h"

#include "FollowingModel.h"
#include "PendingPostService.h"
#include "SidebarService.h"
#include "UserProfileService.h"
#include "WebappPluginService.h"
#include "types/BackendChannel.h"
#include "types/BackendPost.h"

namespace Mattermost {

void Backend::startAccountData()
{
    if (_accountDataStarted || !storage.loginUser || storage.loginUser->id.isEmpty())
        return;
    _accountDataStarted = true;
    // A session's services are initialized before any presentation subscribes.
    // Unresolved sends become local drafts, never automatic retransmissions.
    PendingPostService::instance(*this);
    auto& sidebar = SidebarService::instance(*this);
    sidebar.clear();
    UserProfileService::instance(*this).clear();
    auto& webappPlugins = WebappPluginService::instance(*this);
    webappPlugins.clear();
    webappPlugins.refresh();

    connect(this, &Backend::onChannelViewed, this, [this](const BackendChannel& channel) {
        SidebarService::instance(*this).setChannelMentioned(channel.id, false);
    });
    connect(this, &Backend::onAllTeamChannelsPopulated, this, [this] {
        _accountCatalogReady = true;
        auto& currentSidebar = SidebarService::instance(*this);
        for (auto& team : storage.teams)
            currentSidebar.retrieveCategories(team.second);
        FollowingModel::instance(*this).ensureThreadsFresh();
    });
    connect(this, &Backend::onRealtimeRecovered, this, [this] {
        if (!loggedIn()) return;
        SidebarService::instance(*this).refreshAccountSnapshot();
        FollowingModel::instance(*this).noteThreadUpdated();
    });
    sidebar.retrieveChannelMemberships();
    retrieveOwnTeams([this](BackendTeam& team) { emit ownTeamDiscovered(team); });
}

} // namespace Mattermost
