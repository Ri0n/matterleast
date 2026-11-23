/**
 * @file ChannelTree.h
 * @brief
 * @author Lyubomir Filipov
 * @date Dec 18, 2021
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <QHash>
#include <QMap>
#include <QPair>
#include <QPointer>
#include <QPersistentModelIndex>
#include <QSet>
#include <QStringList>
#include <QTreeWidget>
#include <QVector>

#include "ChannelTreeItem.h"
#include "SidebarItem.h"

class QDragLeaveEvent;
class QDragMoveEvent;
class QDropEvent;
class QEvent;
class QMouseEvent;
class QLabel;
class QTimer;
class QPainter;
class QStackedWidget;
class QTreeWidgetItem;
class QVariantAnimation;
class ChannelTreeAccountIdentityTest;

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendTeam;
class BackendUser;
class ChatArea;
class TeamItem;
class ChannelItem;
class Storage;
class SidebarService;
struct SidebarTeamState;

class ChannelTree: public QTreeWidget {
	Q_OBJECT
public:
    using ItemKind = SidebarItem::Kind;
    using ItemRole = SidebarItem::Role;

    // Compatibility names for the existing ChannelTree implementation and
    // callers. SidebarItem is the canonical contract shared by all sidebar
    // views; new code should use it directly.
    static constexpr ItemKind UnknownItem = SidebarItem::Unknown;
    static constexpr ItemKind TeamItemKind = SidebarItem::Team;
    static constexpr ItemKind CategoryItemKind = SidebarItem::Category;
    static constexpr ItemKind ChannelItemKind = SidebarItem::Channel;
    static constexpr ItemKind VirtualDestinationItemKind = SidebarItem::VirtualDestination;
    static constexpr ItemKind AccountRootItemKind = SidebarItem::AccountRoot;

    static constexpr ItemRole ItemKindRole = SidebarItem::KindRole;
    static constexpr ItemRole ItemIdRole = SidebarItem::IdRole;
    static constexpr ItemRole ItemTeamIdRole = SidebarItem::TeamIdRole;
    static constexpr ItemRole ItemMutedRole = SidebarItem::MutedRole;
    static constexpr ItemRole ItemMentionedRole = SidebarItem::MentionedRole;
    static constexpr ItemRole ItemStatusRole = SidebarItem::PresenceRole;
    static constexpr ItemRole ItemUnreadRole = SidebarItem::UnreadRole;
    static constexpr ItemRole ItemLifetimeRole = SidebarItem::LifetimeRole;
    static constexpr ItemRole ItemChannelTypeRole = SidebarItem::ChannelTypeRole;
    static constexpr ItemRole ItemChannelIdRole = SidebarItem::ChannelIdRole;
    static constexpr ItemRole ItemDestinationRole = SidebarItem::DestinationRole;
    static constexpr ItemRole ItemAccountIdRole = SidebarItem::AccountIdRole;

	ChannelTree (QWidget* parent = nullptr);
	virtual ~ChannelTree ();
public:
    // The existing QWidget tree belongs to one Backend. Stamp its existing
    // model rows with that Backend's durable local account ID; never rebind
    // it to a different account (that requires a separate presentation).
    bool bindSingleAccount(const QString& accountId);
    QString boundAccountId() const { return _boundAccountId; }
    // View-scoped account roots, without a replacement channel model.
    // Singletons retain the exact original team hierarchy.
    // Do not activate merged Views until their shared DM/GM section is ready.
    bool setVisibleAccountRoots(const QStringList& accountIds,
                                const QHash<QString, QString>& labels = {});
    // The AccountManager owns Backend lifetimes; this tree only observes them.
    bool registerAccountBackend(const QString& accountId, Backend& backend);
    // Populate this same tree from an already loaded account snapshot.
    // Must not cause another fetch of the server's sidebar categories.
    void synchronizeAccountSidebar(const QString& accountId);
    bool setMergedConversationSorting(const QString& sorting);
    void refreshMergedConversations();
    void unregisterAccountBackend(const QString& accountId, const Backend* expected = nullptr);
    Backend* backendForAccount(const QString& accountId) const;
    Backend* backendForRow(const QTreeWidgetItem* row) const;
    QString accountIdForBackend(const Backend* backend) const;
	bool isChannelActive (const BackendChannel& channel);
	Backend* backendInstance() const;

    // ChannelItemDelegate requests this lazily when a group-DM row becomes
    // visible. Keeping the request on the tree side avoids backend/network work
    // inside paint() and keeps the delegate independently testable.
    Q_INVOKABLE void ensureGroupChannelDisplayName(const QString& channelId);
    Q_INVOKABLE void ensureGroupChannelDisplayName(const QString& accountId,
                                                 const QString& channelId);

	void addTeam (Backend& backend, BackendTeam& team);
	void populateSidebars(Backend& backend);
    /** Re-fetch read state, preferences and server categories after reconnect. */
    void refreshServerSnapshot();
    void refreshServerSnapshotForAccount(const QString& accountId);
    // Reconcile a temporary, non-persistent view of older conversations while
    // the sidebar search field is populated.
    void setConversationSearchTerm(const QString& term);

	// Kept for source compatibility; the server-backed sidebar no longer uses
	// separate global DM/GM lists.
	void addDirectChannelsList (Backend& backend);
	void addGroupChannelsList (Backend& backend);

	void setChatAreaStackedWidget (QStackedWidget* chatAreaStackedWidget);
	ChatArea* getCurrentPage ();

	void openChannel (QString channelID);
    void openChannelForAccount(QString channelID, const QString& accountId);
	// Programmatic navigation may target a valid channel that is currently
	// outside the visible DM/GM sidebar limit. Materialize its server-category
	// row first, then activate it through the same path as an ordinary click.
	void openStoredChannel(QString channelID);
    void openStoredChannelForAccount(QString channelID, const QString& accountId);
    // A realtime direct/group channel can reach Storage before the server-backed
    // sidebar category is refreshed. Admit it into every loaded Direct Messages
    // category without opening the conversation or issuing a category reload.
    void admitStoredConversation(BackendChannel& channel);
    void updateDirectCustomStatus(const BackendUser& user);
	void addChannelToItem (QString channelID, QTreeWidgetItem* item);
	void removeChannelToItem (QString channelID, QTreeWidgetItem* item = nullptr);

	// Recent and Attention are alternate views over the same channel objects.
	// Route their context-menu requests back through the real ChannelItem so all
	// actions and handlers stay identical to the Channels tab.
	void showChannelContextMenu(const QString& channelID, const QPoint& globalPos)
    {
        if (_visibleAccountRootIds.size() > 1)
            return;
        // Old Recent/Attention widgets belong to a single-account window.
        showChannelContextMenuForAccount(channelID, _boundAccountId, globalPos);
    }

    // Merged projections must supply the originating account: remote
    // channel IDs are not globally unique.
    void showChannelContextMenuForAccount(const QString& channelID,
                                          const QString& accountId,
                                          const QPoint& globalPos)
    {
        if (accountId.isEmpty() && _visibleAccountRootIds.size() > 1)
            return;
        const ScopedId key {accountId.isEmpty() ? _boundAccountId : accountId,
                            channelID};
        Backend* owner = backendForAccount(key.first);
        if (!owner)
            return;
        const auto it = channelToItemMap.constFind(key);
        if (it == channelToItemMap.cend())
            return;
        for (QTreeWidgetItem* item : it.value()) {
            if (!item || item->data(0, ItemKindRole).toInt() != ChannelItemKind
                || backendForRow(item) != owner)
                continue;
            static_cast<ChannelTreeItem*>(item)->showContextMenu(globalPos);
            return;
        }
    }


signals:
    void channelActivated();
    // A View-level sidebar has no ChatArea stack of its own. Delegate
    // navigation to the owning account workspace instead of cloning chats.
    void conversationOpenRequested(const QString& accountId, const QString& channelId);
    void teamSidebarPopulated(const QString& teamId);
    void virtualDestinationRequested(int destination, const QString& teamId);
    /** Completion of a stored-channel open that required asynchronous admission/join. */
    void storedChannelOpenFinished(const QString& channelId, bool opened);

protected:
	void currentChanged(const QModelIndex& current, const QModelIndex& previous) override;
	void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void startDrag(Qt::DropActions supportedActions) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
	void dragMoveEvent(QDragMoveEvent* event) override;
	void dropEvent(QDropEvent* event) override;
    void changeEvent(QEvent* event) override;
    void drawBranches(QPainter* painter, const QRect& rect,
                      const QModelIndex& index) const override;

private:
    static void tagAccount(QTreeWidgetItem* item, const QString& accountId);
    void attachTeamToAccountRoot(QTreeWidgetItem* team);
    void observeSidebarSignals(Backend& backend, SidebarService& sidebar);
    using ScopedId = QPair<QString, QString>;
    // Keep the remote identifier local to its owning account. The row role
    // takes priority for retained/moved items; before login it is unbound.
    ScopedId scopedId(const QString& remoteId) const
    {
        return {_boundAccountId, remoteId};
    }
    ScopedId scopedId(const QString& remoteId, const QTreeWidgetItem* item) const
    {
        const QString account = item
            ? item->data(0, ItemAccountIdRole).toString() : QString();
        return {account.isEmpty() ? _boundAccountId : account, remoteId};
    }
    friend class ChannelTreeDragScrollTest;
    friend class ::ChannelTreeAccountIdentityTest;
	void refreshCurrentChannelReadState(QTreeWidgetItem* item);
	void showContextMenu (const QPoint& pos);
	void handleChannelLeave();
    void handleChannelLeaveForAccount(const QString& accountId, const QString& channelId);
    void handleChannelUpdated();
    void onDirectCustomStatusChanged();
    void bindDirectUser(Backend& backend, BackendUser& user);
    void updateDirectUserVisuals(const BackendUser& user);
	void refreshTeamSidebar(Backend& backend, BackendTeam& team);
	void renderTeamSidebar(Backend& backend, TeamItem& teamItem,
	                       const SidebarTeamState& state);
    void reconcileTeamSidebar(Backend& backend, TeamItem& teamItem,
                              const SidebarTeamState& state);
    void destroySidebarRow(QTreeWidgetItem* item);
    void removeTeamSidebar(const ScopedId& teamKey);
	QTreeWidgetItem* createCategoryItem(TeamItem& teamItem, const QString& categoryId,
	                                    const QString& displayName, bool collapsed);
	ChannelItem* createChannelItem(Backend& backend, TeamItem* teamItem,
	                               QTreeWidgetItem& categoryItem, BackendChannel& channel);
    ChannelItem* createPersonalItem(Backend& backend, TeamItem& teamItem,
                                    QTreeWidgetItem& parentItem);
    ChannelItem* createSavedItem(Backend& backend, TeamItem& teamItem,
                                 QTreeWidgetItem& parentItem);
    ChannelItem* createDraftsItem(Backend& backend, TeamItem& teamItem,
                                  QTreeWidgetItem& parentItem);
    ChannelItem* createRecentMentionsItem(Backend& backend, TeamItem& teamItem,
                                          QTreeWidgetItem& parentItem);
    QTreeWidgetItem* personalItemForTeam(const QString& teamId,
                                        const QString& accountId = {}) const;
    void refreshPersonalItems();
    void activateConversation(const QString& channelId, const QString& teamId = {},
                              const QString& accountId = {});
	void activateChannelItem(QTreeWidgetItem* item);
    void activateVirtualDestination(QTreeWidgetItem* item);
	void setCategoryCollapsed(QTreeWidgetItem* item, bool collapsed);
    void showCategoryMenu(const QString& accountId, const QString& teamId,
                          const QString& categoryId);
	void setChannelMutedVisual(const QString& channelId, bool muted);
    void refreshChannelUnreadVisual(const QString& channelId);
    void refreshAllChannelActivityVisuals();
	void setChannelUnreadVisual(const QString& channelId, bool unread);
	void setChannelMentionedVisual(const QString& channelId, bool mentioned);
	bool resolveChannelDropTarget(QTreeWidgetItem* source, const QPoint& pos,
	                              QTreeWidgetItem*& targetCategoryItem,
	                              QString& targetChannelId, bool& afterTarget) const;
    struct CategoryDragBoundary {
        int position = 0;
        QPersistentModelIndex targetCategory;
        bool afterTarget = false;
    };

    bool resolveCategoryDropTarget(QTreeWidgetItem* source, const QPoint& pos,
                                   QTreeWidgetItem*& targetCategoryItem,
                                   bool& afterTarget) const;
    void prepareCategoryDragBoundaries(QTreeWidgetItem* source);
    bool updateDragTargetAt(const QPoint& viewportPos);
    void tickDragAutoScroll();
    void updateInternalDragPointer(const QPoint& viewportPos);
    void finishInternalDrag(const QPoint& viewportPos, bool commit);
    void scrollDuringDrag(int scrollUnits, const QPoint& viewportPos);
    const CategoryDragBoundary* nearestCategoryDragBoundary(int probe) const;
    void updateDragVisuals(QTreeWidgetItem* source, QTreeWidgetItem* gapAnchor,
                           bool gapAfter);
    void restoreSourceDropGap(bool animate);
    void clearDropGap(bool animate);
    void resetDragVisuals(bool animate);
    void ensureDragSourceVisuals(QTreeWidgetItem* source);
    QTreeWidgetItem* sourceDropGapAnchor(QTreeWidgetItem* source, bool& gapAfter) const;
    void animateSourceCollapse(qreal target);
    void animateDropGap(const QPersistentModelIndex& target, bool after, int extent);
    QTreeWidgetItem* channelDropGapAnchor(QTreeWidgetItem* source,
                                            QTreeWidgetItem* targetCategoryItem,
                                            const QString& targetChannelId,
                                            bool afterTarget, bool& gapAfter) const;
    QTreeWidgetItem* categoryDropGapAnchor(QTreeWidgetItem* targetCategoryItem,
                                             bool afterTarget, bool& gapAfter) const;
	void moveChannel(ChannelItem* item, const QString& targetCategoryId,
	                 const QString& targetChannelId, bool afterTarget,
	                 bool explicitPosition);
    void moveCategory(QTreeWidgetItem* item, const QString& targetCategoryId,
                      bool afterTarget);
	void refreshSidebarTeam(const ScopedId& teamKey);
    void verifySidebarTeam(const ScopedId& teamKey, quint64 mutation);
    void flushDeferredSidebarReconciles();
    Backend* backendForSidebarSignal() const;
    Backend* backendForStorage(const Storage& storage) const;
    Backend* backendForUser(const BackendUser& user) const;
    void onSidebarCategoriesChanged(const QString& teamId);
    void refreshPaletteDependentIcons();

	QStackedWidget*						chatAreaStackedWidget;
    QMap<ScopedId, QPointer<ChatArea>>  channelPages;
    QMap<ScopedId, QList<QTreeWidgetItem*>> channelToItemMap;
    QMap<ScopedId, TeamItem*>            teamToItemMap;
    QSet<ScopedId>                      connectedSidebarUsers;
    QSet<Backend*>                      profileSourcesConnected;
    QSet<ScopedId>                        pendingChannelAdmissions;
    QPointer<Backend>                   backendForSidebar;
    QString                             _boundAccountId;
    QMap<QString, QPointer<Backend>>     _accountBackends;
    QMap<ScopedId, QPointer<BackendTeam>> _observedTeams;
    QTreeWidgetItem* _sharedConversationsRoot = nullptr;
    QString _mergedConversationSorting = QStringLiteral("recent");
    QStringList                         _visibleAccountRootIds;
    QHash<QString, QString>             _accountRootLabels;
    QHash<QString, QTreeWidgetItem*>    _accountRootItems;
	bool							renderingSidebar;
    bool                                personalUserConnected = false;
    // True during an internal sidebar mouse drag. Structural sidebar
    // mutations from network/realtime callbacks are deferred while this is set.
    bool                                sidebarDragActive = false;
    QPointer<QLabel>                    dragPreview;
    QTimer*                             dragScrollTimer = nullptr;
    QPoint                             dragPreviewHotSpot;
    QPersistentModelIndex               categoryActionHoverIndex;
    bool                                categoryActionHovered = false;
    bool                                categoryMenuHovered = false;
    QVariantAnimation*                  sourceCollapseAnimation = nullptr;
    QVariantAnimation*                  dropGapAnimation = nullptr;
    QVector<QPersistentModelIndex>      dragSourceIndexes;
    QVector<QPersistentModelIndex>      dragGapIndexes;
    QPersistentModelIndex               currentDragGapIndex;
    QPersistentModelIndex               sourceDragGapIndex;
    bool                                currentDragGapAfter = false;
    bool                                sourceDragGapAfter = false;
    int                                 currentDragGapExtent = 0;
    int                                 draggedRowExtent = 0;
    int                                 draggedBlockHotSpotY = 0;
    int                                 dragStartPointerY = 0;
    // Cumulative pixel offset, not scrollbar item units: category reorder
    // probes must track the content moving beneath a stationary pointer.
    int                                 dragViewportScrollPixels = 0;
    int                                 draggedBlockStartLogicalY = 0;
    QVector<CategoryDragBoundary>       categoryDragBoundaries;
    QSet<ScopedId>                       pendingSidebarReconcileTeams;
    QSet<ScopedId>                       pendingSidebarRefreshTeams;
    QSet<ScopedId>                       pendingStoredChannelOpens;
    QMap<ScopedId, quint64>              sidebarMutationGeneration;
};

} /* namespace Mattermost */
