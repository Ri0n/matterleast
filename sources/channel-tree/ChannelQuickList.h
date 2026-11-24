/**
 * @file ChannelQuickList.h
 * @brief Followed conversation queue used by the Following sidebar tab.
 */

#pragma once

#include <optional>

#include <QMap>
#include <QSet>
#include <QTreeWidget>

#include "backend/FollowingModel.h"
#include "channel-tree/UnreadProjectionPolicy.h"
#include "channel-tree/SidebarQueueSources.h"

class QKeyEvent;
class QMouseEvent;
class QShowEvent;

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendUser;

class ChannelQuickList : public QTreeWidget
{
    Q_OBJECT
public:
    enum Mode {
        Following,
    };

    explicit ChannelQuickList(QWidget* parent = nullptr);

    void initialize(Backend& backend, Mode mode = Following);
    void setAccounts(const SidebarQueueSources::Accounts& accounts);
    void refresh();
    void refreshThreads();
    void releaseSelectionRetention();
    void setUnreadFilters(const UnreadViewFilters& filters);

signals:
    void channelSelected(const QString& channelId, Backend* source);
    void channelContextMenuRequested(const QString& channelId, const QPoint& globalPos, Backend* source);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void activateItem(QTreeWidgetItem* item);
    void openThread(Backend& owner, const FollowingModel::Entry& entry);
    QString threadLabel(Backend& owner, const FollowingModel::Entry& entry) const;
    void updateDirectUser(const QString& accountId, const BackendUser& user);
    SidebarQueueSources sources_;
    bool refreshing_ = false;
    UnreadViewFilters unreadFilters_;
    QMap<QString, QTreeWidgetItem*> channelItems_;

    QString retainedAccountId_;
    QString retainedKey_;
    QString lastActivatedKey_;
    std::optional<FollowingModel::Entry> retainedEntry_;
    // Only entries from Following may remain briefly after they are read.
    // Channels/Muted-only entries must not become "read Following" rows.
    bool retainedFromFollowing_ = false;
};

} // namespace Mattermost
