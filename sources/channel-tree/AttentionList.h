/**
 * @file AttentionList.h
 * @brief Personal attention projection of the shared Following model.
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <cstdint>
#include <optional>

#include <QTreeWidget>

#include "SidebarItem.h"
#include "backend/FollowingModel.h"
#include "SidebarQueueSources.h"

class QMouseEvent;
class QTimer;

namespace Mattermost {

class Backend;
class BackendUser;

class AttentionList : public QTreeWidget
{
    Q_OBJECT
public:
    explicit AttentionList(QWidget* parent = nullptr);

    void initialize(Backend& backend);
    void setAccounts(const SidebarQueueSources::Accounts& accounts);
    void refresh();
    void refreshThreads();
    void releaseSelectionRetention();

    Backend* backendAt(const QPoint& pos) const
    {
        const auto* item = itemAt(pos);
        return item ? sources_.backend(item->data(0, SidebarItem::AccountIdRole).toString()) : nullptr;
    }

    QString channelIdAt(const QPoint& pos) const
    {
        QTreeWidgetItem* item = itemAt(pos);
        return item ? item->data(0, SidebarItem::ChannelIdRole).toString() : QString();
    }

signals:
    void channelSelected(const QString& channelId, Backend* source);
    /** Repeated selection should flash visible unread posts without scrolling. */
    void conversationAttentionReactivated(const QString& channelId, Backend* source);
    void attentionCountChanged(uint32_t count);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void updateDirectUser(const QString& accountId, const BackendUser& user);
    void activateItem(QTreeWidgetItem* item);
    void retainSelection(QTreeWidgetItem* item);
    void openThread(Backend& owner, const FollowingModel::Entry& entry);
    QString threadLabel(Backend& owner, const FollowingModel::Entry& entry) const;

    SidebarQueueSources sources_;

    // Presentation-only snapshot for keeping the selected row visible.
    // Navigation always resolves the current cursor from FollowingModel.
    QString retainedAccountId_;
    std::optional<FollowingModel::Entry> retainedEntry_;
    QString lastActivatedKey_;

    // Keep the tree geometrically stable for a brief moment after selection
    // moves. The newly selected entry is activated immediately; only the
    // projection rebuild/removal of the old retained row is deferred.
    QTimer* selectionRefreshTimer_ = nullptr;

    bool refreshing_ = false;
    int lastAttentionCount_ = -1;
};

} // namespace Mattermost
