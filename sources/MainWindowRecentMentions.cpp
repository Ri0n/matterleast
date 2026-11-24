#include "mainwindow.h"

#include "ui_mainwindow.h"
#include "channel-tree/SidebarItem.h"
#include "post-collection/PostCollectionView.h"

namespace Mattermost {

void MainWindow::on_channelList_virtualDestinationRequested(
    int destination, const QString& teamId)
{
    if (!backendOrNull() || destination != SidebarItem::RecentMentionsDestination) {
        return;
    }
    Q_UNUSED(teamId)

    if (!recentMentionsPage || &recentMentionsPage->backendInstance() != backendOrNull()) {
        recentMentionsPage = nullptr;
        for (auto* page : ui->chatAreaStackedWidget->findChildren<PostCollectionView*>(
                QString(), Qt::FindDirectChildrenOnly)) {
            if (&page->backendInstance() == backendOrNull()
                && page->viewMode() == PostCollectionView::Mode::RecentMentions) {
                recentMentionsPage = page;
                break;
            }
        }
    }
    if (!recentMentionsPage) {
        recentMentionsPage = new PostCollectionView(
            backendInstance(), PostCollectionView::Mode::RecentMentions,
            ui->chatAreaStackedWidget);
    }
    showCollectionPage(recentMentionsPage);
    recentMentionsPage->activateRecentMentions();
}

} // namespace Mattermost
