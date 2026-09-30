#include "mainwindow.h"

#include "ui_mainwindow.h"
#include "channel-tree/SidebarItem.h"
#include "post-collection/PostCollectionView.h"

namespace Mattermost {

void MainWindow::on_channelList_virtualDestinationRequested(
    int destination, const QString& teamId)
{
    if (destination != SidebarItem::RecentMentionsDestination) {
        return;
    }
    Q_UNUSED(teamId)

    if (!recentMentionsPage) {
        recentMentionsPage = new PostCollectionView(
            backend, PostCollectionView::Mode::RecentMentions,
            ui->chatAreaStackedWidget);
    }
    showCollectionPage(recentMentionsPage);
    recentMentionsPage->activateRecentMentions();
}

} // namespace Mattermost
