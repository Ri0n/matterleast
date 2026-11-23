/**
 * @file ChannelTree.cpp
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

#include "ChannelTree.h"

#include <QAbstractItemModel>
#include <QAction>
#include <QApplication>
#include <QCursor>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QStackedWidget>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <utility>
#include <QItemSelectionModel>
#include <QScopedValueRollback>
#include <QSignalBlocker>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/SharedConversationProjection.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "ui/UserStatusEmoji.h"
#include "backend/types/BackendTeam.h"
#include "chat-area/ChatArea.h"
#include "channel-tree/ChannelIcons.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "channel-tree/SidebarChannelMovePolicy.h"
#include "channel-tree/SidebarManualOrder.h"
#include "ui/IconUtils.h"
#include "channel-tree/SidebarDragScrollPolicy.h"
#include "channel-tree/channel-item/DirectChannelItem.h"
#include "channel-tree/team-item/GroupTeamItem.h"
#include "log.h"

namespace Mattermost {

namespace {

QPoint dropEventPosition(const QDropEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position().toPoint();
#else
    return event->pos();
#endif
}

QRect dragContentRect(const QTreeWidgetItem* item, QRect rect)
{
    if (!item) {
        return rect;
    }
    rect.adjust(0, qMax(0, item->data(0, SidebarItem::DropGapBeforeRole).toInt()),
                0, -qMax(0, item->data(0, SidebarItem::DropGapAfterRole).toInt()));
    return rect;
}

bool channelDropChangesPosition(QTreeWidgetItem* source,
                                QTreeWidgetItem* targetCategory,
                                const QString& targetChannelId,
                                bool afterTarget)
{
    if (!source || !targetCategory) {
        return false;
    }
    if (source->parent() != targetCategory) {
        return true;
    }

    QStringList channelIds;
    for (int i = 0; i < targetCategory->childCount(); ++i) {
        QTreeWidgetItem* row = targetCategory->child(i);
        if (row && row->data(0, SidebarItem::KindRole).toInt() == SidebarItem::Channel) {
            channelIds.push_back(row->data(0, SidebarItem::IdRole).toString());
        }
    }
    return reorderSidebarChannel(channelIds,
                                 source->data(0, SidebarItem::IdRole).toString(),
                                 targetChannelId, afterTarget);
}

bool categoryDropChangesPosition(QTreeWidgetItem* source,
                                 QTreeWidgetItem* targetCategory,
                                 bool afterTarget)
{
    if (!source || !targetCategory || source->parent() != targetCategory->parent()) {
        return false;
    }

    QTreeWidgetItem* team = source->parent();
    QStringList categoryIds;
    for (int i = 0; team && i < team->childCount(); ++i) {
        QTreeWidgetItem* row = team->child(i);
        if (row && row->data(0, SidebarItem::KindRole).toInt() == SidebarItem::Category) {
            categoryIds.push_back(row->data(0, SidebarItem::IdRole).toString());
        }
    }
    return reorderSidebarCategory(categoryIds,
                                  source->data(0, SidebarItem::IdRole).toString(),
                                  targetCategory->data(0, SidebarItem::IdRole).toString(),
                                  afterTarget);
}

class VirtualDestinationItem final : public ChannelItem
{
public:
    using ChannelItem::ChannelItem;

    void showContextMenu(const QPoint&) override {}
};

} // namespace

ChannelTree::ChannelTree (QWidget* parent)
:QTreeWidget (parent)
,chatAreaStackedWidget(nullptr)
,backendForSidebar(nullptr)
,renderingSidebar(false)
{
	connect (this, &QTreeWidget::customContextMenuRequested, this, &ChannelTree::showContextMenu);

	connect (this, &QTreeWidget::currentItemChanged, this,
             [this] (QTreeWidgetItem* item, QTreeWidgetItem*) {
        // Reconciliation can temporarily change selection while moving rows;
        // never interpret those internal updates as user navigation.
        if (!renderingSidebar) {
            activateChannelItem(item);
        }
    });

    connect(this, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) {
        if (!renderingSidebar) {
            setCategoryCollapsed(item, false);
        }
    });
    connect(this, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) {
        if (!renderingSidebar) {
            setCategoryCollapsed(item, true);
        }
    });

    auto* categoryDelegate = new ChannelItemDelegate(this);
    setItemDelegate(categoryDelegate);
    connect(categoryDelegate, &ChannelItemDelegate::categoryMenuRequested,
            this, &ChannelTree::showCategoryMenu);
    setMouseTracking(true);
    setDragEnabled(true);
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    setDropIndicatorShown(false);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);

    // New server categories, DMs and virtual destinations can arrive long
    // after the MainWindow is bound to a durable account. Stamp inserted
    // QTreeWidget rows in place; this does not change their identity within
    // the existing tree or replace its Qt item model.
    connect(model(), &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex& parent, int first, int last) {
        const QString accountId = parent.isValid()
            ? parent.data(ItemAccountIdRole).toString() : _boundAccountId;
        if (accountId.isEmpty())
            return;
        for (int row = first; row <= last; ++row) {
            QTreeWidgetItem* item = itemFromIndex(model()->index(row, 0, parent));
            if (item)
                tagAccount(item, accountId);
        }
    });
}

void ChannelTree::tagAccount(QTreeWidgetItem* item, const QString& accountId)
{
    if (!item)
        return;
    // Retained rows may already be explicitly bound to another account.
    QString actualId = item->data(0, ItemAccountIdRole).toString();
    if (actualId.isEmpty()) {
        actualId = accountId;
        item->setData(0, ItemAccountIdRole, actualId);
    }
    for (int i = 0; i < item->childCount(); ++i)
        tagAccount(item->child(i), actualId);
}

bool ChannelTree::bindSingleAccount(const QString& accountId)
{
    if (accountId.isEmpty()
        || (!_boundAccountId.isEmpty() && _boundAccountId != accountId))
        return false;
    if (_boundAccountId == accountId)
        return true;
    // Preflight the binding before touching any live indices or model roles.
    if (backendForSidebar && !registerAccountBackend(accountId, *backendForSidebar))
        return false;

    // Login can bind an already populated sidebar. Re-key live objects in
    // place; do not reconstruct rows, chat widgets or selection.
    auto rekey = [&accountId](auto& map) {
        auto old = std::move(map);
        map.clear();
        for (auto it = old.cbegin(); it != old.cend(); ++it)
            map.insert({accountId, it.key().second}, it.value());
    };
    rekey(teamToItemMap);
    rekey(channelToItemMap);
    rekey(channelPages);
    QSet<ScopedId> reboundUsers;
    for (const ScopedId& entry : connectedSidebarUsers)
        reboundUsers.insert({accountId, entry.second});
    connectedSidebarUsers = std::move(reboundUsers);
    _boundAccountId = accountId;
    // A login can finish after initial channel rows were materialized.
    // Reuse them and their ChatArea/selection state; do not rebuild the tree.
    for (int i = 0; i < topLevelItemCount(); ++i)
        tagAccount(topLevelItem(i), _boundAccountId);
    return true;
}

bool ChannelTree::setSharedConversationSorting(const QString& sorting)
{
    if (sorting != QLatin1String("recent") && sorting != QLatin1String("alpha"))
        return false;
    _sharedConversationSorting = sorting;
    return true;
}

QVector<SharedConversationOrder::Entry> ChannelTree::sharedConversationEntries() const
{
    if (_visibleAccountRootIds.size() < 2)
        return {};

    // One account Backend is one source. Teams and category snapshots are
    // irrelevant to the identity or visibility of its direct conversations.
    QVector<SharedConversationProjection::AccountSource> sources;
    for (const QString& accountId : _visibleAccountRootIds) {
        if (Backend* owner = backendForAccount(accountId))
            sources.push_back({accountId, owner});
    }
    return SharedConversationProjection::collect(sources, _sharedConversationSorting);
}

bool ChannelTree::setVisibleAccountRoots(const QStringList& accountIds,
                                         const QHash<QString, QString>& labels)
{
    // An active drag owns the tree structure until drop/cancel.
    if (sidebarDragActive)
        return false;
    QSet<QString> seen;
    for (const QString& id : accountIds) {
        if (id.isEmpty() || seen.contains(id))
            return false;
        seen.insert(id);
    }
    if (_visibleAccountRootIds == accountIds) {
        _accountRootLabels = labels;
        for (auto it = _accountRootItems.cbegin(); it != _accountRootItems.cend(); ++it) {
            const QString name = labels.value(it.key(), it.key());
            if (it.value()->text(0) != name)
                it.value()->setText(0, name);
        }
        return true;
    }

    // Qt item ownership/parenting changes; channel, team, page and model
    // addresses remain stable. Suppress temporary selection-as-navigation.
    QTreeWidgetItem* const selectedRow = currentItem();
    // Account headers are destroyed when switching presentation modes.
    // Never keep their raw pointers alive across the reparenting.
    QTreeWidgetItem* const selected = selectedRow
        && selectedRow->data(0, ItemKindRole).toInt() != AccountRootItemKind
        ? selectedRow : nullptr;
    const bool selectedBefore = selected && selected->isSelected();
    const QSignalBlocker selectionBlock(this);
    const QScopedValueRollback<bool> renderGuard(renderingSidebar, true);

    for (QTreeWidgetItem* root : _accountRootItems) {
        while (root->childCount())
            addTopLevelItem(root->takeChild(0));
        const int index = indexOfTopLevelItem(root);
        if (index >= 0)
            takeTopLevelItem(index);
        delete root;
    }
    _accountRootItems.clear();
    _visibleAccountRootIds = accountIds;
    _accountRootLabels = labels;

    if (accountIds.size() > 1) {
        int index = 0;
        for (const QString& accountId : accountIds) {
            auto* root = new QTreeWidgetItem;
            root->setData(0, ItemKindRole, AccountRootItemKind);
            root->setData(0, ItemIdRole, accountId);
            root->setData(0, ItemAccountIdRole, accountId);
            root->setText(0, labels.value(accountId, accountId));
            root->setFlags(root->flags()
                           & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled
                               | Qt::ItemIsEditable));
            QFont font = root->font(0);
            font.setBold(true);
            root->setFont(0, font);
            insertTopLevelItem(index++, root);
            root->setExpanded(true);
            _accountRootItems.insert(accountId, root);
        }
    }
    for (int i = topLevelItemCount() - 1; i >= 0; --i) {
        QTreeWidgetItem* row = topLevelItem(i);
        if (row && row->data(0, ItemKindRole).toInt() == TeamItemKind)
            attachTeamToAccountRoot(row);
    }

    bool stillVisible = selected && selected->treeWidget() == this;
    for (QTreeWidgetItem* ancestor = selected; stillVisible && ancestor;
         ancestor = ancestor->parent()) {
        if (ancestor->isHidden())
            stillVisible = false;
    }
    if (stillVisible) {
        setCurrentItem(selected, 0, selectedBefore
            ? QItemSelectionModel::ClearAndSelect : QItemSelectionModel::NoUpdate);
    } else {
        // Switching Views must not leave an invisible conversation selected.
        setCurrentItem(nullptr, 0, QItemSelectionModel::Clear);
    }
    return true;
}

void ChannelTree::attachTeamToAccountRoot(QTreeWidgetItem* team)
{
    if (!team || team->data(0, ItemKindRole).toInt() != TeamItemKind)
        return;
    const QString accountId = team->data(0, ItemAccountIdRole).toString();
    const bool visible = _visibleAccountRootIds.isEmpty()
        || _visibleAccountRootIds.contains(accountId);
    if (QTreeWidgetItem* root = _accountRootItems.value(accountId, nullptr)) {
        if (team->parent() != root) {
            if (team->parent())
                team->parent()->removeChild(team);
            else if (const int index = indexOfTopLevelItem(team); index >= 0)
                takeTopLevelItem(index);
            root->addChild(team);
        }
    }
    team->setHidden(!visible);
}

bool ChannelTree::registerAccountBackend(const QString& accountId, Backend& backend)
{
    if (accountId.isEmpty())
        return false;
    const auto existing = _accountBackends.constFind(accountId);
    if (existing != _accountBackends.cend()
        && existing.value() && existing.value() != &backend)
        return false;
    for (auto it = _accountBackends.cbegin(); it != _accountBackends.cend(); ++it) {
        if (it.key() != accountId && it.value() == &backend)
            return false;
    }
    if (accountId == _boundAccountId && backendForSidebar
        && backendForSidebar != &backend)
        return false;
    _accountBackends.insert(accountId, &backend);
    return true;
}

void ChannelTree::unregisterAccountBackend(const QString& accountId,
                                            const Backend* expected)
{
    const auto it = _accountBackends.constFind(accountId);
    if (it == _accountBackends.cend() || (expected && it.value() != expected))
        return;
    _accountBackends.remove(accountId);
    for (auto userIt = connectedSidebarUsers.begin();
         userIt != connectedSidebarUsers.end();) {
        if (userIt->first == accountId)
            userIt = connectedSidebarUsers.erase(userIt);
        else
            ++userIt;
    }
    if (accountId == _boundAccountId
        && (!expected || expected == backendForSidebar))
        backendForSidebar = nullptr;
}

Backend* ChannelTree::backendForAccount(const QString& accountId) const
{
    if (accountId.isEmpty()) {
        if (_visibleAccountRootIds.size() > 1)
            return nullptr;
        // Legacy callers can omit the account only while exactly one Backend
        // owns this presentation. A bound primary account does not make an
        // unqualified lookup safe once another Backend is registered.
        Backend* legacy = backendForSidebar.data();
        if (!legacy)
            return nullptr;
        for (auto it = _accountBackends.cbegin(); it != _accountBackends.cend(); ++it) {
            if (it.value() && it.value() != legacy)
                return nullptr;
        }
        return legacy;
    }
    const auto it = _accountBackends.constFind(accountId);
    if (it != _accountBackends.cend())
        return it.value();
    return accountId == _boundAccountId ? backendForSidebar.data() : nullptr;
}

Backend* ChannelTree::backendForStorage(const Storage& storage) const
{
    for (auto it = _accountBackends.cbegin(); it != _accountBackends.cend(); ++it) {
        Backend* owner = it.value();
        if (owner && &owner->getStorage() == &storage)
            return owner;
    }
    return backendForSidebar && &backendForSidebar->getStorage() == &storage
        ? backendForSidebar.data() : nullptr;
}

Backend* ChannelTree::backendForUser(const BackendUser& user) const
{
    for (auto it = _accountBackends.cbegin(); it != _accountBackends.cend(); ++it) {
        Backend* owner = it.value();
        if (owner && owner->getStorage().getUserById(user.id) == &user)
            return owner;
    }
    return backendForSidebar
        && backendForSidebar->getStorage().getUserById(user.id) == &user
        ? backendForSidebar.data() : nullptr;
}

Backend* ChannelTree::backendForRow(const QTreeWidgetItem* row) const
{
    if (row) {
        const QString id = row->data(0, ItemAccountIdRole).toString();
        if (!id.isEmpty())
            return backendForAccount(id);
    }
    // Legacy/unbound single-account presentation before account adoption.
    return backendForAccount({});
}

QString ChannelTree::accountIdForBackend(const Backend* backend) const
{
    if (!backend)
        return {};
    for (auto it = _accountBackends.cbegin(); it != _accountBackends.cend(); ++it) {
        if (it.value() == backend)
            return it.key();
    }
    return backend == backendForSidebar ? _boundAccountId : QString();
}

ChannelTree::~ChannelTree()
{
    // A sidebar can be destroyed by a workspace/window change while a mouse
    // drag is active. The preview belongs to the top-level window, not to the
    // tree, and must not linger after the source view disappears.
    if (dragScrollTimer) {
        dragScrollTimer->stop();
    }
    if (qApp) {
        qApp->removeEventFilter(this);
    }
    delete dragPreview.data();
}

bool ChannelTree::isChannelActive (const BackendChannel& channel)
{
	return getCurrentPage () && &getCurrentPage()->getChannel() == &channel;
}

void ChannelTree::addTeam (Backend& backend, BackendTeam& team)
{
    if (!backendForSidebar)
        backendForSidebar = &backend;
    if (!profileSourcesConnected.contains(&backend)) {
        profileSourcesConnected.insert(&backend);
        QPointer<Backend> ownerGuard(&backend);
        connect(&backend, &QObject::destroyed, this, [this, source = &backend] {
            profileSourcesConnected.remove(source);
        });
        connect(&UserProfileService::instance(backend),
                &UserProfileService::profileResolved, this,
                [this, ownerGuard](const QString& userId) {
            if (!ownerGuard)
                return;
            auto& storage = ownerGuard->getStorage();
            auto* user = storage.getUserById(userId);
            const auto* channel = storage.getDirectChannelByUserId(userId);
            const ScopedId key {accountIdForBackend(ownerGuard.data()),
                                channel ? channel->id : QString()};
            if (!user || !channel || !channelToItemMap.contains(key))
                return;
            bindDirectUser(*ownerGuard.data(), *user);
            updateDirectUserVisuals(*user);
            updateDirectCustomStatus(*user);
        });
        connect(&backend.emojiRegistry(), &EmojiRegistry::customEmojiAdded,
                this, [this, ownerGuard](const QString& name) {
            if (!ownerGuard)
                return;
            const QString accountId = accountIdForBackend(ownerGuard.data());
            for (const ScopedId& entry : connectedSidebarUsers) {
                if (entry.first != accountId)
                    continue;
                const BackendUser* user =
                    ownerGuard->getStorage().getUserById(entry.second);
                if (user && UserStatusEmoji::name(*user) == name)
                    updateDirectCustomStatus(*user);
            }
        });
    }

	TeamItem* teamList = new GroupTeamItem (*this, backend, team.display_name, team.id);
    teamList->setData(0, ItemKindRole, TeamItemKind);
    teamList->setData(0, ItemIdRole, team.id);
    teamList->setData(0, ItemTeamIdRole, team.id);
    const QString accountId = accountIdForBackend(&backend);
    if (!accountId.isEmpty())
        teamList->setData(0, ItemAccountIdRole, accountId);
    teamList->setFlags(teamList->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));

	addTopLevelItem (teamList);
    attachTeamToAccountRoot(teamList);
    teamToItemMap.insert({accountId, team.id}, teamList);
	header()->setSectionResizeMode(0, QHeaderView::Stretch);

	connect (&team, &BackendTeam::onNewChannel, this, [this, &backend, &team] (BackendChannel&) {
        refreshTeamSidebar(backend, team);
	});

    connect(&team, &BackendTeam::onLeave, this, [this, &backend, teamId = team.id] {
        const ScopedId key {accountIdForBackend(&backend), teamId};
        if (sidebarDragActive) {
            pendingSidebarRefreshTeams.insert(key);
            return;
        }
        removeTeamSidebar(key);
    });

    // Backend loads the account-wide channel catalog after announcing teams.
    // Team members are still fetched only by dialogs that need them.
}

void ChannelTree::setConversationSearchTerm(const QString& term)
{
    const auto applyToBackend = [this, &term](Backend* owner) {
        if (!owner)
            return;
        const QString id = accountIdForBackend(owner);
        if (!_visibleAccountRootIds.isEmpty() && !_visibleAccountRootIds.contains(id))
            return;
        auto& sidebar = SidebarService::instance(*owner);
        const bool wasSearching = sidebar.conversationSearchActive();
        sidebar.setConversationSearchTerm(term);

        // Leaving search admits the current DM/GM into this account's own
        // categories without disturbing selection or the other accounts.
        if (wasSearching && !sidebar.conversationSearchActive()) {
            BackendChannel* current = owner->getCurrentChannel();
            if (current && (current->type == BackendChannel::directChannel
                            || current->type == BackendChannel::groupChannel))
                admitStoredConversation(*current);
        }

        const QString accountId = accountIdForBackend(owner);
        for (auto it = teamToItemMap.cbegin(); it != teamToItemMap.cend(); ++it) {
            TeamItem* team = it.value();
            if (it.key().first != accountId || !team || &team->backend != owner)
                continue;
            const SidebarTeamState* state = sidebar.teamState(it.key().second);
            if (state)
                reconcileTeamSidebar(*owner, *team, *state);
        }
    };
    // Avoid a temporary model or duplicate storage when a View contains
    // multiple Backends. The familiar singleton path remains unchanged.
    applyToBackend(backendForSidebar.data());
    for (auto it = _accountBackends.cbegin(); it != _accountBackends.cend(); ++it) {
        if (it.value() && it.value() != backendForSidebar.data())
            applyToBackend(it.value().data());
    }
}

Backend* ChannelTree::backendForSidebarSignal() const
{
    const QObject* source = sender();
    if (!source)
        return _visibleAccountRootIds.size() > 1 ? nullptr : backendForSidebar.data();
    for (auto it = _accountBackends.cbegin(); it != _accountBackends.cend(); ++it) {
        Backend* owner = it.value();
        if (owner && &SidebarService::instance(*owner) == source)
            return owner;
    }
    if (backendForSidebar
        && &SidebarService::instance(*backendForSidebar) == source)
        return backendForSidebar.data();
    return nullptr;
}

void ChannelTree::onSidebarCategoriesChanged(const QString& teamId)
{
    Backend* owner = backendForSidebarSignal();
    if (!owner)
        return;
    TeamItem* team = teamToItemMap.value({accountIdForBackend(owner), teamId}, nullptr);
    const SidebarTeamState* state = SidebarService::instance(*owner).teamState(teamId);
    if (team && state)
        reconcileTeamSidebar(*owner, *team, *state);
}

void ChannelTree::populateSidebars(Backend& backend)
{
    if (!backendForSidebar)
        backendForSidebar = &backend;
    auto& sidebar = SidebarService::instance(backend);
    connect(&sidebar, &SidebarService::channelMutedChanged,
            this, &ChannelTree::setChannelMutedVisual, Qt::UniqueConnection);
    connect(&sidebar, &SidebarService::channelActivityChanged,
            this, &ChannelTree::refreshChannelUnreadVisual, Qt::UniqueConnection);
    connect(&sidebar, &SidebarService::channelActivityReset,
            this, &ChannelTree::refreshAllChannelActivityVisuals,
            Qt::UniqueConnection);
    connect(&sidebar, &SidebarService::channelMentionedChanged,
            this, &ChannelTree::setChannelMentionedVisual, Qt::UniqueConnection);
    connect(&sidebar, &SidebarService::categoriesChanged, this,
            &ChannelTree::onSidebarCategoriesChanged, Qt::UniqueConnection);

    for (auto it = teamToItemMap.cbegin(); it != teamToItemMap.cend(); ++it) {
        // Multiple servers can legitimately use the same remote team ID.
        if (it.value() && &it.value()->backend == &backend) {
            if (BackendTeam* team = backend.getStorage().getTeamById(it.key().second))
                refreshTeamSidebar(backend, *team);
        }
    }
}

void ChannelTree::refreshServerSnapshot()
{
    if (_visibleAccountRootIds.size() > 1)
        return;
    refreshServerSnapshotForAccount(_boundAccountId);
}

void ChannelTree::refreshServerSnapshotForAccount(const QString& accountId)
{
    Backend* owner = backendForAccount(accountId);
    if (!owner)
        return;
    const QString id = accountId.isEmpty() ? _boundAccountId : accountId;
    auto& sidebar = SidebarService::instance(*owner);
    sidebar.invalidateCachedReads();
    QPointer<ChannelTree> guard(this);
    QPointer<Backend> ownerGuard(owner);
    sidebar.retrieveChannelMemberships([guard, ownerGuard, id] {
        if (!guard || !ownerGuard
            || guard->backendForAccount(id) != ownerGuard.data())
            return;
        auto& currentSidebar = SidebarService::instance(*ownerGuard.data());
        currentSidebar.retrieveChannelPreferences([guard, ownerGuard, id] {
            if (!guard || !ownerGuard
                || guard->backendForAccount(id) != ownerGuard.data())
                return;
            // Read cursors alone cannot update unread state. Refresh the
            // originating server's channel summaries before its categories.
            SidebarService::instance(*ownerGuard.data())
                .refreshKnownChannelSummaries([guard, ownerGuard, id] {
                    if (!guard || !ownerGuard
                        || guard->backendForAccount(id) != ownerGuard.data())
                        return;
                    const QList<ScopedId> teamKeys = guard->teamToItemMap.keys();
                    for (const ScopedId& key : teamKeys) {
                        if (key.first == id)
                            guard->refreshSidebarTeam(key);
                    }
                });
        });
    });
}

void ChannelTree::refreshSidebarTeam(const ScopedId& teamKey)
{
    Backend* owner = backendForAccount(teamKey.first);
    if (!owner)
        return;
    BackendTeam* team = owner->getStorage().getTeamById(teamKey.second);
    if (team)
        refreshTeamSidebar(*owner, *team);
    else
        removeTeamSidebar(teamKey);
}

void ChannelTree::refreshTeamSidebar(Backend& backend, BackendTeam& team)
{
    const QString teamId = team.id;
    QPointer<ChannelTree> treeGuard(this);
    QPointer<Backend> backendGuard(&backend);
    SidebarService::instance(backend).retrieveCategories(team,
        [treeGuard, backendGuard, teamId](const SidebarTeamState& state) {
            // A delayed server response must not resurrect a disposed sidebar
            // or apply another Backend's matching remote team ID.
            if (!treeGuard || !backendGuard)
                return;
            BackendTeam* liveTeam = backendGuard->getStorage().getTeamById(teamId);
            const ScopedId key {treeGuard->accountIdForBackend(backendGuard.data()), teamId};
            TeamItem* item = treeGuard->teamToItemMap.value(key, nullptr);
            if (!liveTeam || !item || &item->backend != backendGuard.data())
                return;
            treeGuard->renderTeamSidebar(*backendGuard.data(), *item, state);
            emit treeGuard->teamSidebarPopulated(teamId);
        });
}

void ChannelTree::renderTeamSidebar(Backend& backend, TeamItem& teamItem,
                                    const SidebarTeamState& state)
{
    reconcileTeamSidebar(backend, teamItem, state);
}

void ChannelTree::removeTeamSidebar(const ScopedId& teamKey)
{
    TeamItem* teamItem = teamToItemMap.take(teamKey);
    if (!teamItem) {
        return;
    }

    const QSignalBlocker selectionGuard(this);
    const QScopedValueRollback<bool> renderingGuard(renderingSidebar, true);
    const bool removingCurrent = currentItem()
        && currentItem()->data(0, ItemTeamIdRole).toString() == teamKey.second
        && currentItem()->data(0, ItemAccountIdRole).toString() == teamKey.first;
    while (teamItem->childCount() > 0) {
        QTreeWidgetItem* row = teamItem->takeChild(0);
        if (!row) {
            continue;
        }

        if (row->data(0, ItemKindRole).toInt() == CategoryItemKind) {
            while (row->childCount() > 0) {
                destroySidebarRow(row->takeChild(0));
            }
            delete row;
        } else {
            destroySidebarRow(row);
        }
    }
    delete teamItem;
    if (removingCurrent) {
        setCurrentItem(nullptr, 0, QItemSelectionModel::Clear);
    }
}

QTreeWidgetItem* ChannelTree::createCategoryItem(TeamItem& teamItem, const QString& categoryId,
                                                 const QString& displayName, bool collapsed)
{
    auto* item = new QTreeWidgetItem(&teamItem, QStringList() << displayName);
    item->setData(0, ItemKindRole, CategoryItemKind);
    item->setData(0, ItemIdRole, categoryId);
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setFlags((item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled)
                   & ~Qt::ItemIsEditable);

    QFont font = item->font(0);
    font.setBold(true);
    item->setFont(0, font);
    item->setExpanded(!collapsed);
    return item;
}

ChannelItem* ChannelTree::createPersonalItem(Backend& backend, TeamItem& teamItem,
                                             QTreeWidgetItem& parentItem)
{
    auto* item = new DirectChannelItem(backend, nullptr);
    parentItem.addChild(item);
    item->setData(0, ItemKindRole, VirtualDestinationItemKind);
    item->setData(0, ItemIdRole, QStringLiteral("virtual:personal"));
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setData(0, ItemDestinationRole, SidebarItem::PersonalDestination);
    item->setData(0, ItemChannelTypeRole, BackendChannel::directChannel);
    item->setFlags(item->flags()
                   & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsEditable));
    item->setLabel(tr("Personal"));

    if (BackendChannel* channel = backend.getStorage().getDirectChannelByUserId(
            backend.getLoginUser().id)) {
        item->setData(0, ItemChannelIdRole, channel->id);
    }

    const BackendUser& self = backend.getLoginUser();
    item->setStatus(self.status);
    if (!self.avatar.isNull()) {
        item->setIcon(QIcon(self.avatar));
    }

    if (!personalUserConnected) {
        personalUserConnected = true;
        if (self.avatar.isNull()) {
            backend.retrieveUserAvatar(self.id);
        }
        connect(&self, &BackendUser::onAvatarChanged, this,
                [this] { refreshPersonalItems(); });
        connect(&self, &BackendUser::onStatusChanged, this,
                [this] { refreshPersonalItems(); });
    }
    return item;
}

ChannelItem* ChannelTree::createSavedItem(Backend& backend, TeamItem& teamItem,
                                          QTreeWidgetItem& parentItem)
{
    auto* item = new VirtualDestinationItem(backend, nullptr);
    parentItem.addChild(item);
    item->setData(0, ItemKindRole, VirtualDestinationItemKind);
    item->setData(0, ItemIdRole, QStringLiteral("virtual:saved"));
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setData(0, ItemDestinationRole, SidebarItem::SavedDestination);
    item->setFlags(item->flags()
                   & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsEditable));
    item->setLabel(tr("Saved"));
    item->setIcon(QIcon(QStringLiteral(":/icons/bookmark")));
    return item;
}

ChannelItem* ChannelTree::createDraftsItem(Backend& backend, TeamItem& teamItem,
                                           QTreeWidgetItem& parentItem)
{
    auto* item = new VirtualDestinationItem(backend, nullptr);
    parentItem.addChild(item);
    item->setData(0, ItemKindRole, VirtualDestinationItemKind);
    item->setData(0, ItemIdRole, QStringLiteral("virtual:drafts"));
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setData(0, ItemDestinationRole, SidebarItem::DraftsDestination);
    item->setFlags(item->flags()
                   & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled | Qt::ItemIsEditable));
    item->setLabel(tr("Drafts"));
    item->setIcon(QIcon(QStringLiteral(":/icons/edit")));
    return item;
}

QTreeWidgetItem* ChannelTree::personalItemForTeam(const QString& teamId,
                                                  const QString& accountId) const
{
    const ScopedId key {accountId.isEmpty() ? _boundAccountId : accountId, teamId};
    TeamItem* teamItem = teamToItemMap.value(key, nullptr);
    if (!teamItem) {
        return nullptr;
    }

    for (int rowIndex = 0; rowIndex < teamItem->childCount(); ++rowIndex) {
        QTreeWidgetItem* row = teamItem->child(rowIndex);
        if (row
            && row->data(0, ItemKindRole).toInt() == VirtualDestinationItemKind
            && row->data(0, ItemDestinationRole).toInt()
                == SidebarItem::PersonalDestination) {
            return row;
        }
    }
    return nullptr;
}

void ChannelTree::refreshPersonalItems()
{
    for (auto it = teamToItemMap.cbegin(); it != teamToItemMap.cend(); ++it) {
        Backend* owner = backendForAccount(it.key().first);
        if (!owner)
            continue;
        const BackendUser& self = owner->getLoginUser();
        BackendChannel* channel = owner->getStorage().getDirectChannelByUserId(self.id);
        QTreeWidgetItem* row = personalItemForTeam(it.key().second, it.key().first);
        if (!row)
            continue;
        auto* personalItem = static_cast<ChannelItem*>(row);
        personalItem->setStatus(self.status);
        personalItem->setIcon(self.avatar.isNull() ? QIcon() : QIcon(self.avatar));
        row->setData(0, ItemChannelIdRole, channel ? channel->id : QString());
    }
}

ChannelItem* ChannelTree::createChannelItem(Backend& backend, TeamItem& teamItem,
                                            QTreeWidgetItem& categoryItem, BackendChannel& channel)
{
    ChannelItem* item = nullptr;
    if (channel.type == BackendChannel::directChannel || channel.type == BackendChannel::groupChannel) {
        item = new DirectChannelItem(backend, nullptr);
    } else {
        item = teamItem.createChannelItem(backend, nullptr);
    }

    categoryItem.addChild(item);
    item->setData(0, ItemKindRole, ChannelItemKind);
    item->setData(0, ItemIdRole, channel.id);
    item->setData(0, ItemTeamIdRole, teamItem.teamId);
    item->setFlags((item->flags() | Qt::ItemIsDragEnabled) & ~Qt::ItemIsDropEnabled);
    item->setLabel(channel.display_name);

    if (channel.type != BackendChannel::directChannel) {
        item->setIcon(ChannelIcons::conversation(channel.type));
    }

    // A channel may appear in multiple sidebar categories and category refreshes
    // routinely destroy/recreate the QTreeWidgetItem objects. Never capture a
    // row pointer in the long-lived BackendChannel::onLeave connection. The
    // member slot resolves the currently alive rows from channelToItemMap.
    QObject::connect(&channel, &BackendChannel::onLeave,
                     this, &ChannelTree::handleChannelLeave,
                     Qt::UniqueConnection);
    QObject::connect(&channel, &BackendChannel::onUpdated,
                     this, &ChannelTree::handleChannelUpdated,
                     Qt::UniqueConnection);

    addChannelToItem(channel.id, item);
    if (channel.type == BackendChannel::directChannel) {
        if (auto* user = backend.getStorage().getUserById(channel.name)) {
            bindDirectUser(backend, *user);
            item->setStatus(user->status);
            item->setIcon(QIcon(user->avatar));
            item->setData(0, SidebarItem::CustomStatusEmojiRole,
                          UserStatusEmoji::pixmap(backend.emojiRegistry(), *user, 18, font()));
        }
    }
    auto& sidebar = SidebarService::instance(backend);
    item->setMuted(sidebar.isChannelMuted(channel));
    item->setUnread(sidebar.isChannelUnread(channel));
    item->setMentioned(sidebar.hasUnreadMention(channel.id));
    return item;
}

void ChannelTree::bindDirectUser(Backend& owner, BackendUser& user)
{
    const ScopedId key {accountIdForBackend(&owner), user.id};
    if (connectedSidebarUsers.contains(key))
        return;
    connectedSidebarUsers.insert(key);
    QPointer<Backend> ownerGuard(&owner);
    connect(&user, &QObject::destroyed, this, [this, ownerGuard, id = user.id] {
        if (ownerGuard)
            connectedSidebarUsers.remove({accountIdForBackend(ownerGuard.data()), id});
    });
    connect(&user, &BackendUser::onCustomStatusChanged,
            this, &ChannelTree::onDirectCustomStatusChanged);
    connect(&user, &BackendUser::onStatusChanged, this, [this, &user] {
        updateDirectUserVisuals(user);
    });
    connect(&user, &BackendUser::onAvatarChanged, this, [this, &user] {
        updateDirectUserVisuals(user);
    });
    auto& profiles = UserProfileService::instance(owner);
    profiles.ensureAvatar(user);
    if (user.status.isEmpty() && owner.loggedIn())
        profiles.ensureStatuses({user.id});
}

void ChannelTree::updateDirectUserVisuals(const BackendUser& user)
{
    Backend* owner = backendForUser(user);
    if (!owner)
        return;
    const auto* channel = owner->getStorage().getDirectChannelByUserId(user.id);
    if (!channel)
        return;
    const QIcon avatar(user.avatar);
    for (QTreeWidgetItem* row : channelToItemMap.value(
             {accountIdForBackend(owner), channel->id})) {
        if (row && row->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            auto* item = static_cast<ChannelItem*>(row);
            item->setStatus(user.status);
            item->setIcon(avatar);
        }
    }
}

void ChannelTree::onDirectCustomStatusChanged()
{
    auto* user = qobject_cast<BackendUser*>(sender());
    if (user) updateDirectCustomStatus(*user);
}

void ChannelTree::updateDirectCustomStatus(const BackendUser& user)
{
    Backend* owner = backendForUser(user);
    if (!owner)
        return;
    const BackendChannel* channel = owner->getStorage().getDirectChannelByUserId(user.id);
    if (!channel)
        return;
    const QPixmap image = UserStatusEmoji::pixmap(
        owner->emojiRegistry(), user, 18, font());
    for (QTreeWidgetItem* row : channelToItemMap.value(
             {accountIdForBackend(owner), channel->id})) {
        if (row && row->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            row->setData(0, SidebarItem::CustomStatusEmojiRole, image);
        }
    }
}

void ChannelTree::handleChannelLeave()
{
    auto* channel = qobject_cast<BackendChannel*>(sender());
    if (!channel)
        return;

    // Resolve the concrete Storage owner: remote channel IDs can collide.
    if (Backend* owner = backendForStorage(channel->storage))
        handleChannelLeaveForAccount(accountIdForBackend(owner), channel->id);
}

void ChannelTree::handleChannelLeaveForAccount(const QString& accountId,
                                               const QString& channelId)
{
    const ScopedId key {accountId, channelId};
    // Storage can destroy the channel immediately after onLeave(), including
    // while a drag defers removal of its rows.
    if (ChatArea* page = channelPages.take(key)) {
        chatAreaStackedWidget->removeWidget(page);
        delete page;
    }

    const QList<QTreeWidgetItem*> items = channelToItemMap.value(key);
    if (sidebarDragActive) {
        for (QTreeWidgetItem* item : items) {
            QTreeWidgetItem* category = item ? item->parent() : nullptr;
            if (!category) {
                continue;
            }
            const QString teamId = category->data(0, ItemTeamIdRole).toString();
            if (!teamId.isEmpty()) {
                pendingSidebarRefreshTeams.insert({accountId, teamId});
            }
        }
        return;
    }

    const QSignalBlocker selectionGuard(this);
    const QScopedValueRollback<bool> renderingGuard(renderingSidebar, true);
    for (QTreeWidgetItem* item : items) {
        destroySidebarRow(item);
    }
}

void ChannelTree::activateConversation(const QString& channelId, const QString& teamId,
                                       const QString& accountId)
{
    Backend* owner = backendForAccount(accountId);
    if (!owner || !chatAreaStackedWidget)
        return;
    const ScopedId key {accountId.isEmpty() ? _boundAccountId : accountId, channelId};
    BackendChannel* channel = owner->getStorage().getChannelById(channelId);
    if (!channel) {
        return;
    }

    ChatArea* currentPage = getCurrentPage();
    ChatArea* page = channelPages.value(key);
    if (!page) {
        const QString context = teamId.isEmpty() ? owner->getCurrentTeamContextId()
                                                 : teamId;
        page = new ChatArea(*owner, *channel, context, chatAreaStackedWidget, false);
        channelPages.insert(key, page);
        connect(channel, &BackendChannel::onLeave, this,
                &ChannelTree::handleChannelLeave, Qt::UniqueConnection);
        chatAreaStackedWidget->addWidget(page);
    } else if (!teamId.isEmpty()) {
        page->sidebarTeamId = teamId;
    }

    if (currentPage && currentPage != page) {
        currentPage->onDeactivate();
    }
    chatAreaStackedWidget->setCurrentWidget(page);
    // The first widget can already be current after addWidget(). Activation
    // still initializes it and sets the backend's active channel.
    page->onActivate();
    emit channelActivated();
}

void ChannelTree::activateChannelItem(QTreeWidgetItem* item)
{
    if (!item) {
        return;
    }

    const int kind = item->data(0, ItemKindRole).toInt();
    if (kind == VirtualDestinationItemKind) {
        activateVirtualDestination(item);
        return;
    }
    if (kind != ChannelItemKind) {
        return;
    }

    Backend* owner = backendForRow(item);
    const QString accountId = item->data(0, ItemAccountIdRole).toString();
    if (owner) {
        BackendChannel* personal = owner->getStorage().getDirectChannelByUserId(
            owner->getLoginUser().id);
        if (personal && item->data(0, ItemIdRole).toString() == personal->id) {
            if (QTreeWidgetItem* alias = personalItemForTeam(
                    item->data(0, ItemTeamIdRole).toString(), accountId)) {
                setCurrentItem(alias);
                return;
            }
        }
    }

    // A filtered DM/GM already has a selectable search row. Admitting it
    // into the persistent sidebar here would reconcile the QTreeWidget in
    // the next event-loop turn, after Qt finishes the click, and can move the
    // selected highlight to another row. Defer admission until search ends.

    activateConversation(item->data(0, ItemIdRole).toString(),
                         item->data(0, ItemTeamIdRole).toString(), accountId);
}

void ChannelTree::activateVirtualDestination(QTreeWidgetItem* item)
{
    if (!item || item->data(0, ItemKindRole).toInt() != VirtualDestinationItemKind)
        return;
    Backend* owner = backendForRow(item);
    if (!owner)
        return;
    const QString accountId = item->data(0, ItemAccountIdRole).toString();
    const int destination = item->data(0, ItemDestinationRole).toInt();
    if (destination == SidebarItem::SavedDestination
        || destination == SidebarItem::DraftsDestination
        || destination == SidebarItem::RecentMentionsDestination) {
        emit virtualDestinationRequested(destination,
                                         item->data(0, ItemTeamIdRole).toString());
        return;
    }
    if (destination != SidebarItem::PersonalDestination) {
        return;
    }

    BackendChannel* channel = owner->getStorage().getDirectChannelByUserId(
        owner->getLoginUser().id);
    if (!channel) {
        const QString teamId = item->data(0, ItemTeamIdRole).toString();
        QPointer<ChannelTree> guard(this);
        QPointer<Backend> ownerGuard(owner);
        owner->createDirectChannel(owner->getLoginUser(),
            [guard, ownerGuard, teamId, accountId](BackendChannel& created) {
                if (!guard || !ownerGuard
                    || guard->backendForAccount(accountId) != ownerGuard.data())
                    return;
                QTreeWidgetItem* currentPersonal = guard->personalItemForTeam(
                    teamId, accountId);
                if (!currentPersonal)
                    return;
                currentPersonal->setData(0, ItemChannelIdRole, created.id);
                guard->refreshPersonalItems();
                // Only the selected destination may activate after a reply.
                if (guard->currentItem() == currentPersonal)
                    guard->activateVirtualDestination(currentPersonal);
            });
        return;
    }

    item->setData(0, ItemChannelIdRole, channel->id);
    activateConversation(channel->id, item->data(0, ItemTeamIdRole).toString(),
                         accountId);
}

void ChannelTree::addGroupChannelsList (Backend& backend)
{
    Q_UNUSED(backend);
}

void ChannelTree::addDirectChannelsList (Backend& backend)
{
    Q_UNUSED(backend);
}

void ChannelTree::setChatAreaStackedWidget (QStackedWidget* stackedWidget)
{
	chatAreaStackedWidget = stackedWidget;
}

void ChannelTree::openChannel(QString channelID)
{
    if (_visibleAccountRootIds.size() > 1)
        return;
    openChannelForAccount(std::move(channelID), _boundAccountId);
}

void ChannelTree::openChannelForAccount(QString channelID, const QString& accountId)
{
    if (accountId.isEmpty() && _visibleAccountRootIds.size() > 1)
        return;
    const ScopedId key {accountId.isEmpty() ? _boundAccountId : accountId, channelID};
    Backend* owner = backendForAccount(key.first);
    auto it = channelToItemMap.find(key);
    QTreeWidgetItem* item = nullptr;

    if (it != channelToItemMap.end() && !it.value().isEmpty()) {
        item = it.value().front();
    } else if (owner) {
        BackendChannel* personal = owner->getStorage().getDirectChannelByUserId(
            owner->getLoginUser().id);
        if (personal && personal->id == channelID) {
            // Programmatic navigation (permalinks, Saved/Search results, etc.)
            // must resolve the self-DM even when its ordinary server row is
            // absent or suppressed in Favorites. Prefer the currently selected
            // team's Personal row, otherwise use the first available one.
            QTreeWidgetItem* current = currentItem();
            QTreeWidgetItem* teamItem = current;
            while (teamItem && teamItem->data(0, ItemKindRole).toInt()
                        != TeamItemKind) {
                teamItem = teamItem->parent();
            }
            if (teamItem && teamItem->data(0, ItemKindRole).toInt() == TeamItemKind
                && teamItem->data(0, ItemAccountIdRole).toString() == key.first) {
                item = personalItemForTeam(
                    teamItem->data(0, ItemTeamIdRole).toString(), key.first);
            }
            if (!item) {
                for (auto teamIt = teamToItemMap.cbegin(); teamIt != teamToItemMap.cend(); ++teamIt) {
                    if (teamIt.key().first != key.first)
                        continue;
                    item = personalItemForTeam(teamIt.key().second, key.first);
                    if (item)
                        break;
                }
            }
        }
    }

    if (!item) {
        if (channelPages.value(key)) {
            setCurrentItem(nullptr, 0, QItemSelectionModel::Clear);
            activateConversation(channelID, {}, key.first);
        } else {
            qDebug() << "openChannel " << channelID << ": channel not found";
        }
        return;
    }

    if (currentItem() == item) {
        activateChannelItem(item);
    } else {
        setCurrentItem(item);
    }
}

void ChannelTree::addChannelToItem (QString channelID, QTreeWidgetItem* item)
{
	auto& items = channelToItemMap[scopedId(channelID, item)];
	if (!items.contains(item)) {
		items.push_back(item);
	}
}

void ChannelTree::removeChannelToItem (QString channelID, QTreeWidgetItem* item)
{
	auto it = channelToItemMap.find(scopedId(channelID, item));
	if (it == channelToItemMap.end()) {
		return;
	}

	if (item) {
		it.value().removeAll(item);
	} else {
		it.value().clear();
	}

	if (it.value().isEmpty()) {
		channelToItemMap.erase(it);
	}
}

void ChannelTree::showContextMenu (const QPoint& pos)
{
	QTreeWidgetItem* item = itemAt(pos);
    if (!item) {
        return;
    }
    if (item->data(0, ItemKindRole).toInt() == CategoryItemKind) {
        showCategoryMenu(item->data(0, ItemAccountIdRole).toString(),
                         item->data(0, ItemTeamIdRole).toString(),
                         item->data(0, ItemIdRole).toString());
        return;
    }

    const int kind = item->data(0, ItemKindRole).toInt();
    if (kind != TeamItemKind && kind != ChannelItemKind) {
        return;
    }

    auto* pointedItem = static_cast<ChannelTreeItem*>(item);
	pointedItem->showContextMenu (mapToGlobal(pos) + QPoint (25, 15));
}

ChatArea* ChannelTree::getCurrentPage ()
{
    if (!chatAreaStackedWidget) {
        return nullptr;
    }
	return qobject_cast<ChatArea*> (chatAreaStackedWidget->currentWidget());
}

void ChannelTree::showCategoryMenu(const QString& accountId, const QString& teamId,
                                   const QString& categoryId)
{
    Backend* owner = backendForAccount(accountId);
    if (!owner || teamId.isEmpty() || categoryId.isEmpty())
        return;
    QPointer<Backend> ownerGuard(owner);
    auto& sidebar = SidebarService::instance(*owner);
    const SidebarTeamState* state = sidebar.teamState(teamId);
    const SidebarCategory* category = state ? state->category(categoryId) : nullptr;
    if (!category) {
        return;
    }

    const bool custom = category->isCustom();
    const bool muted = category->muted;
    const bool collapsed = category->collapsed;
    const QString currentSorting = category->sorting.isEmpty()
        ? (category->type == QLatin1String("direct_messages")
            ? QStringLiteral("recent") : QStringLiteral("alpha"))
        : category->sorting;

    QMenu menu(this);
    QAction* read = menu.addAction(
        IconUtils::symbolicIcon(QStringLiteral(":/icons/text-check")),
        tr("Mark category as read"));
    // Mattermost's Direct Messages category has its own sorting menu and
    // intentionally does not offer category-wide Mute.
    QAction* mute = category->type == QLatin1String("direct_messages")
        ? nullptr
        : menu.addAction(
            IconUtils::symbolicIcon(QStringLiteral(":/icons/bell")),
            muted ? tr("Unmute category") : tr("Mute category"));
    QAction* collapse = menu.addAction(
        collapsed ? tr("Expand category") : tr("Collapse category"));
    if (custom) {
        menu.addSeparator();
    }
    QAction* rename = custom
        ? menu.addAction(IconUtils::symbolicIcon(QStringLiteral(":/icons/edit")),
                         tr("Rename category")) : nullptr;
    QAction* remove = custom
        ? menu.addAction(IconUtils::symbolicIcon(QStringLiteral(":/icons/trash")),
                         tr("Delete category")) : nullptr;

    menu.addSeparator();
    const QString sortLabel = currentSorting == QLatin1String("manual")
        ? tr("Manually") : currentSorting == QLatin1String("recent")
            ? tr("Recent activity") : tr("Alphabetically");
    QMenu* sortMenu = menu.addMenu(tr("Sort: %1").arg(sortLabel));
    QAction* alpha = sortMenu->addAction(tr("Alphabetically"));
    QAction* recent = sortMenu->addAction(tr("Recent activity"));
    QAction* manual = category->type == QLatin1String("direct_messages")
        ? nullptr : sortMenu->addAction(tr("Manually"));
    for (QAction* action : {alpha, recent}) {
        action->setCheckable(true);
    }
    if (manual) {
        manual->setCheckable(true);
    }
    alpha->setChecked(currentSorting == QLatin1String("alpha"));
    recent->setChecked(currentSorting == QLatin1String("recent"));
    if (manual) {
        manual->setChecked(currentSorting == QLatin1String("manual"));
    }

    menu.addSeparator();
    QAction* create = menu.addAction(
        IconUtils::symbolicIcon(QStringLiteral(":/icons/add")),
        tr("Create category"));

    QAction* chosen = menu.exec(QCursor::pos());
    // Modal menus run a nested event loop: the owning account can disconnect
    // while it is open. Never apply its choice to another server.
    if (!chosen || !ownerGuard || backendForAccount(accountId) != ownerGuard.data())
        return;

    // The server may have delivered a new category snapshot while the menu's
    // modal event loop was running. Resolve again by stable category id.
    SidebarTeamState* liveState = sidebar.teamState(teamId);
    const SidebarCategory* liveCategory = liveState
        ? liveState->category(categoryId) : nullptr;
    if (!liveCategory) {
        return;
    }
    const auto update = [this, &sidebar](SidebarCategory changed) {
        QPointer<ChannelTree> guard(this);
        sidebar.updateCategory(changed, {}, [guard] {
            if (guard) {
                QMessageBox::warning(guard, QObject::tr("Category update"),
                                     QObject::tr("Could not update this category."));
            }
        });
    };

    if (chosen == read) {
        QPointer<ChannelTree> guard(this);
        sidebar.markCategoryRead(teamId, categoryId, [guard](bool success) {
            if (!success && guard) {
                QMessageBox::warning(guard, QObject::tr("Mark category as read"),
                                     QObject::tr("Could not mark the category as read."));
            }
        });
    } else if (chosen == mute) {
        SidebarCategory changed = *liveCategory;
        changed.muted = !changed.muted;
        update(std::move(changed));
    } else if (chosen == collapse) {
        QTreeWidgetItem* row = nullptr;
        TeamItem* team = teamToItemMap.value(
            {accountId.isEmpty() ? _boundAccountId : accountId, teamId}, nullptr);
        for (int i = 0; team && i < team->childCount(); ++i) {
            QTreeWidgetItem* candidate = team->child(i);
            if (candidate->data(0, ItemIdRole).toString() == categoryId) {
                row = candidate;
                break;
            }
        }
        if (row) {
            row->setExpanded(liveCategory->collapsed);
        } else {
            SidebarCategory changed = *liveCategory;
            changed.collapsed = !changed.collapsed;
            update(std::move(changed));
        }
    } else if (chosen == rename && custom) {
        bool accepted = false;
        const QString name = QInputDialog::getText(
            this, tr("Rename category"), tr("Category name:"),
            QLineEdit::Normal, liveCategory->displayName, &accepted).trimmed();
        if (accepted && !name.isEmpty() && name != liveCategory->displayName) {
            SidebarCategory changed = *liveCategory;
            changed.displayName = name;
            update(std::move(changed));
        }
    } else if (chosen == remove && custom) {
        // A visually empty category can still own hidden/capped conversations.
        // Skip confirmation only when the SERVER membership list is empty.
        const bool empty = liveCategory->channelIds.isEmpty();
        if (empty || QMessageBox::question(
                this, tr("Delete category"),
                tr("Delete this category? Its conversations will return to their default categories."))
            == QMessageBox::Yes) {
            QPointer<ChannelTree> guard(this);
            sidebar.deleteCategory(teamId, categoryId, [guard](bool success) {
                if (!success && guard) {
                    QMessageBox::warning(guard, QObject::tr("Delete category"),
                                         QObject::tr("Could not delete this category."));
                }
            });
        }
    } else if (chosen == alpha || chosen == recent
               || (manual && chosen == manual)) {
        const QString sorting = chosen == alpha ? QStringLiteral("alpha")
            : chosen == recent ? QStringLiteral("recent")
                               : QStringLiteral("manual");
        if (liveCategory->sorting == sorting) {
            return;
        }
        SidebarCategory changed = *liveCategory;
        if (sorting == QLatin1String("manual")
            && changed.sorting != QLatin1String("manual")) {
            // Materialize the displayed alpha/recent order BEFORE the mode
            // switch, otherwise the arbitrary server channel_ids reorder rows.
            changed.channelIds = sidebarManualOrder(
                changed.channelIds, sidebar.visibleChannelIds(*liveCategory));
        }
        changed.sorting = sorting;
        update(std::move(changed));
    } else if (chosen == create) {
        bool accepted = false;
        const QString name = QInputDialog::getText(
            this, tr("Create category"), tr("Category name:"),
            QLineEdit::Normal, QString(), &accepted).trimmed();
        if (accepted && !name.isEmpty()) {
            QPointer<ChannelTree> guard(this);
            // The category is placed above the one whose menu was used,
            // and that order is persisted with the server's /order endpoint.
            sidebar.createCategory(teamId, name, {}, categoryId, [guard] {
                if (guard) {
                    QMessageBox::warning(
                        guard, QObject::tr("Create category"),
                        QObject::tr("Could not create or position this category."));
                }
            });
        }
    }
}

void ChannelTree::setCategoryCollapsed(QTreeWidgetItem* item, bool collapsed)
{
    if (!item || item->data(0, ItemKindRole).toInt() != CategoryItemKind)
        return;
    Backend* owner = backendForRow(item);
    if (!owner)
        return;

    const QString teamId = item->data(0, ItemTeamIdRole).toString();
    const QString categoryId = item->data(0, ItemIdRole).toString();
    SidebarTeamState* state = SidebarService::instance(*owner).teamState(teamId);
    SidebarCategory* category = state ? state->category(categoryId) : nullptr;
    if (!category || category->collapsed == collapsed) {
        return;
    }

    category->collapsed = collapsed;
    SidebarService::instance(*owner).updateCategory(*category);
}

void ChannelTree::setChannelMutedVisual(const QString& channelId, bool muted)
{
    Backend* owner = backendForSidebarSignal();
    if (!owner)
        return;
    const auto items = channelToItemMap.value({accountIdForBackend(owner), channelId});
    for (QTreeWidgetItem* item : items) {
        if (item && item->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            static_cast<ChannelItem*>(item)->setMuted(muted);
        }
    }
}

void ChannelTree::refreshChannelUnreadVisual(const QString& channelId)
{
    Backend* owner = backendForSidebarSignal();
    if (!owner)
        return;
    BackendChannel* channel = owner->getStorage().getChannelById(channelId);
    if (!channel) {
        return;
    }

    setChannelUnreadVisual(
        channelId, SidebarService::instance(*owner).isChannelUnread(*channel));
}

void ChannelTree::refreshAllChannelActivityVisuals()
{
    Backend* owner = backendForSidebarSignal();
    if (!owner)
        return;
    const QString accountId = accountIdForBackend(owner);
    auto& sidebar = SidebarService::instance(*owner);
    const QList<ScopedId> channelIds = channelToItemMap.keys();
    for (const ScopedId& identity : channelIds) {
        if (identity.first != accountId)
            continue;
        const QString& channelId = identity.second;
        BackendChannel* channel = owner->getStorage().getChannelById(channelId);
        if (!channel) {
            continue;
        }

        setChannelUnreadVisual(
            channelId, sidebar.isChannelUnread(*channel));
        setChannelMentionedVisual(
            channelId, sidebar.hasUnreadMention(channelId));
    }
}

void ChannelTree::setChannelUnreadVisual(const QString& channelId, bool unread)
{
    Backend* owner = backendForSidebarSignal();
    if (!owner)
        return;
    const auto items = channelToItemMap.value({accountIdForBackend(owner), channelId});
    for (QTreeWidgetItem* item : items) {
        if (item && item->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            static_cast<ChannelItem*>(item)->setUnread(unread);
        }
    }
}

void ChannelTree::setChannelMentionedVisual(const QString& channelId, bool mentioned)
{
    Backend* owner = backendForSidebarSignal();
    if (!owner)
        return;
    const auto items = channelToItemMap.value({accountIdForBackend(owner), channelId});
    for (QTreeWidgetItem* item : items) {
        if (item && item->data(0, ItemKindRole).toInt() == ChannelItemKind) {
            static_cast<ChannelItem*>(item)->setMentioned(mentioned);
        }
    }
}

void ChannelTree::moveChannel(ChannelItem* item,
                              const QString& targetCategoryId,
                              const QString& targetChannelId,
                              bool afterTarget,
                              bool explicitPosition)
{
    if (!item || !item->parent() || targetCategoryId.isEmpty())
        return;
    Backend* owner = backendForRow(item);
    if (!owner)
        return;

    QTreeWidgetItem* sourceCategoryItem = item->parent();
    const QString teamId = sourceCategoryItem->data(0, ItemTeamIdRole).toString();
    const ScopedId teamKey = scopedId(teamId, item);
    auto& sidebar = SidebarService::instance(*owner);
    SidebarTeamState* state = sidebar.teamState(teamId);
    if (!state) {
        return;
    }

    const QString sourceCategoryId = sourceCategoryItem->data(0, ItemIdRole).toString();
    const QString channelId = item->data(0, ItemIdRole).toString();
    const SidebarCategory* sourceCategory = state->category(sourceCategoryId);
    const SidebarCategory* targetCategory = state->category(targetCategoryId);
    if (!sourceCategory || !targetCategory || targetCategory->teamId != teamId
        || channelId.isEmpty()) {
        return;
    }

    const SidebarTeamState before = *state;
    SidebarTeamState optimistic = before;
    SidebarCategory* sourceUpdate = optimistic.category(sourceCategoryId);
    SidebarCategory* targetUpdate = optimistic.category(targetCategoryId);
    if (!sourceUpdate || !targetUpdate) {
        return;
    }

    bool changed = false;
    const bool sameCategory = sourceCategoryId == targetCategoryId;
    if (sameCategory) {
        // DnD is relative to the visible sorted order, NOT the server's
        // channel_ids order, which may be arbitrary in an alpha/recent group.
        if (sourceUpdate->sorting != QLatin1String("manual")) {
            sourceUpdate->channelIds = sidebarManualOrder(
                sourceUpdate->channelIds, sidebar.visibleChannelIds(*sourceCategory));
        }
        changed = reorderSidebarChannel(sourceUpdate->channelIds, channelId,
                                        targetChannelId, afterTarget);
        if (changed) {
            sourceUpdate->sorting = QStringLiteral("manual");
        }
    } else {
        if (explicitPosition && targetUpdate->sorting != QLatin1String("manual")) {
            targetUpdate->channelIds = sidebarManualOrder(
                targetUpdate->channelIds, sidebar.visibleChannelIds(*targetCategory));
        }
        changed = moveSidebarChannel(sourceUpdate->channelIds, targetUpdate->channelIds,
                                     channelId, targetChannelId, afterTarget);
        if (changed && explicitPosition) {
            targetUpdate->sorting = QStringLiteral("manual");
        }
    }
    if (!changed) {
        return;
    }

    const quint64 mutation = ++sidebarMutationGeneration[teamKey];
    sidebar.applyLocalTeamState(teamId, optimistic);
    if (TeamItem* teamItem = teamToItemMap.value(teamKey, nullptr)) {
        if (const SidebarTeamState* local = sidebar.teamState(teamId))
            reconcileTeamSidebar(*owner, *teamItem, *local);
    }

    QPointer<ChannelTree> guard(this);
    QPointer<Backend> ownerGuard(owner);
    auto verify = [guard, ownerGuard, teamKey, mutation] {
        if (!guard || !ownerGuard
            || guard->backendForAccount(teamKey.first) != ownerGuard.data()
            || guard->sidebarMutationGeneration.value(teamKey) != mutation)
            return;
        guard->verifySidebarTeam(teamKey, mutation);
    };
    auto rollback = [guard, ownerGuard, teamKey, mutation, before] {
        if (!guard || !ownerGuard
            || guard->backendForAccount(teamKey.first) != ownerGuard.data()
            || guard->sidebarMutationGeneration.value(teamKey) != mutation)
            return;
        auto& currentSidebar = SidebarService::instance(*ownerGuard.data());
        currentSidebar.applyLocalTeamState(teamKey.second, before);
        if (TeamItem* teamItem = guard->teamToItemMap.value(teamKey, nullptr))
            guard->reconcileTeamSidebar(*ownerGuard.data(), *teamItem, before);
        // The rollback is immediate; the GET is only an authoritative check
        // and is reconciled incrementally when it arrives.
        guard->verifySidebarTeam(teamKey, mutation);
    };

    if (sameCategory) {
        const SidebarCategory payload = *optimistic.category(sourceCategoryId);
        sidebar.updateCategory(
            payload,
            [verify](const SidebarCategory&) { verify(); },
            rollback,
            false);
    } else {
        const QVector<SidebarCategory> payload {
            *optimistic.category(sourceCategoryId),
            *optimistic.category(targetCategoryId),
        };
        sidebar.updateCategories(
            teamId, payload,
            [verify](const SidebarTeamState&) { verify(); },
            rollback,
            false);
    }
}

bool ChannelTree::resolveChannelDropTarget(QTreeWidgetItem* source,
                                           const QPoint& pos,
                                           QTreeWidgetItem*& targetCategoryItem,
                                           QString& targetChannelId,
                                           bool& afterTarget) const
{
    targetCategoryItem = nullptr;
    targetChannelId.clear();
    afterTarget = false;
    if (!source || source->data(0, ItemKindRole).toInt() != ChannelItemKind) {
        return false;
    }

    QTreeWidgetItem* target = itemAt(pos);
    if (!target || target == source) {
        return false;
    }

    const int targetKind = target->data(0, ItemKindRole).toInt();
    if (targetKind == ChannelItemKind) {
        targetCategoryItem = target->parent();
        targetChannelId = target->data(0, ItemIdRole).toString();
        const QRect rect = dragContentRect(target, visualItemRect(target));
        afterTarget = rect.isValid() && pos.y() >= rect.center().y();
    } else if (targetKind == CategoryItemKind) {
        targetCategoryItem = target;
    } else if (targetKind == VirtualDestinationItemKind) {
        // The leading local block has no server category ordinal. A drop on a
        // destination is therefore rejected rather than translated into a
        // Mattermost category/channel mutation.
        return false;
    } else {
        return false;
    }

    QTreeWidgetItem* sourceCategoryItem = source->parent();
    QTreeWidgetItem* sourceTeamItem = sourceCategoryItem ? sourceCategoryItem->parent() : nullptr;
    return targetCategoryItem
        && targetCategoryItem->data(0, ItemKindRole).toInt() == CategoryItemKind
        && backendForRow(targetCategoryItem) == backendForRow(source)
        && backendForRow(source) != nullptr
        && sourceTeamItem
        && targetCategoryItem->parent() == sourceTeamItem;
}

void ChannelTree::moveCategory(QTreeWidgetItem* item,
                               const QString& targetCategoryId,
                               bool afterTarget)
{
    if (!item || targetCategoryId.isEmpty()
        || item->data(0, ItemKindRole).toInt() != CategoryItemKind)
        return;
    Backend* owner = backendForRow(item);
    if (!owner)
        return;

    const QString teamId = item->data(0, ItemTeamIdRole).toString();
    const QString categoryId = item->data(0, ItemIdRole).toString();
    const ScopedId teamKey = scopedId(teamId, item);
    auto& sidebar = SidebarService::instance(*owner);
    SidebarTeamState* state = sidebar.teamState(teamId);
    if (!state || categoryId.isEmpty() || !state->category(categoryId)
        || !state->category(targetCategoryId)) {
        return;
    }

    const SidebarTeamState before = *state;
    SidebarTeamState optimistic = before;
    if (!reorderSidebarCategory(optimistic.order, categoryId,
                                targetCategoryId, afterTarget)) {
        return;
    }

    const quint64 mutation = ++sidebarMutationGeneration[teamKey];
    sidebar.applyLocalTeamState(teamId, optimistic);
    if (TeamItem* teamItem = teamToItemMap.value(teamKey, nullptr))
        reconcileTeamSidebar(*owner, *teamItem, optimistic);

    QPointer<ChannelTree> guard(this);
    QPointer<Backend> ownerGuard(owner);
    auto rollback = [guard, ownerGuard, teamKey, mutation, before] {
        if (!guard || !ownerGuard
            || guard->backendForAccount(teamKey.first) != ownerGuard.data()
            || guard->sidebarMutationGeneration.value(teamKey) != mutation)
            return;
        auto& currentSidebar = SidebarService::instance(*ownerGuard.data());
        currentSidebar.applyLocalTeamState(teamKey.second, before);
        if (TeamItem* teamItem = guard->teamToItemMap.value(teamKey, nullptr))
            guard->reconcileTeamSidebar(*ownerGuard.data(), *teamItem, before);
        guard->verifySidebarTeam(teamKey, mutation);
    };

    sidebar.updateCategoryOrder(
        teamId, optimistic.order,
        [guard, ownerGuard, teamKey, mutation] {
            if (guard && ownerGuard
                && guard->backendForAccount(teamKey.first) == ownerGuard.data()
                && guard->sidebarMutationGeneration.value(teamKey) == mutation)
                guard->verifySidebarTeam(teamKey, mutation);
        },
        rollback,
        false);
}

bool ChannelTree::resolveCategoryDropTarget(QTreeWidgetItem* source,
                                            const QPoint& pos,
                                            QTreeWidgetItem*& targetCategoryItem,
                                            bool& afterTarget) const
{
    targetCategoryItem = nullptr;
    afterTarget = false;
    if (!source || source->data(0, ItemKindRole).toInt() != CategoryItemKind
        || !source->parent() || categoryDragBoundaries.isEmpty()) {
        return false;
    }

    // Match AnyKeep's GenericReorderController: compare the dragged block's
    // leading edge against insertion boundaries computed after removing the
    // source. Nearest-boundary selection means a neighbour moves once the
    // leading edge has crossed half of that neighbour's extent. Moving up and
    // down therefore naturally use the two different half-overlap thresholds.
    const int probe = SidebarDragScrollPolicy::categoryProbe(
        draggedBlockStartLogicalY, pos.y(), dragStartPointerY,
        dragViewportScrollPixels);
    const CategoryDragBoundary* boundary = nearestCategoryDragBoundary(probe);
    if (!boundary || !boundary->targetCategory.isValid()) {
        return false;
    }

    targetCategoryItem = itemFromIndex(boundary->targetCategory);
    afterTarget = boundary->afterTarget;
    return targetCategoryItem
        && targetCategoryItem->data(0, ItemKindRole).toInt() == CategoryItemKind
        && backendForRow(targetCategoryItem) == backendForRow(source)
        && backendForRow(source) != nullptr
        && targetCategoryItem->parent() == source->parent();
}

bool ChannelTree::updateDragTargetAt(const QPoint& pos)
{
    // Called for both native dragMove events and scrolling driven by our own
    // timer/wheel handler. Do not call QTreeWidget::dragMoveEvent(): its
    // internal drop indicator would conflict with our animated structural gap.
    if (!sidebarDragActive) {
        return false;
    }

    QTreeWidgetItem* source = nullptr;
    if (!dragSourceIndexes.isEmpty() && dragSourceIndexes.front().isValid()) {
        source = itemFromIndex(dragSourceIndexes.front());
    }
    if (!source) {
        const auto selected = selectedItems();
        source = selected.size() == 1 ? selected.front() : currentItem();
    }

    if (source && source->data(0, ItemKindRole).toInt() == CategoryItemKind) {
        QTreeWidgetItem* targetCategoryItem = nullptr;
        bool afterTarget = false;
        if (!resolveCategoryDropTarget(source, pos, targetCategoryItem, afterTarget)) {
            restoreSourceDropGap(true);
            return false;
        }
        if (!categoryDropChangesPosition(source, targetCategoryItem, afterTarget)) {
            ensureDragSourceVisuals(source);
            restoreSourceDropGap(true);
            return true;
        }
        bool gapAfter = afterTarget;
        QTreeWidgetItem* anchor = categoryDropGapAnchor(
            targetCategoryItem, afterTarget, gapAfter);
        updateDragVisuals(source, anchor, gapAfter);
        return true;
    }

    QTreeWidgetItem* targetCategoryItem = nullptr;
    QString targetChannelId;
    bool afterTarget = false;
    if (!resolveChannelDropTarget(source, pos, targetCategoryItem,
                                  targetChannelId, afterTarget)) {
        restoreSourceDropGap(true);
        return false;
    }

    if (!channelDropChangesPosition(source, targetCategoryItem,
                                    targetChannelId, afterTarget)) {
        ensureDragSourceVisuals(source);
        restoreSourceDropGap(true);
        return true;
    }
    bool gapAfter = afterTarget;
    QTreeWidgetItem* anchor = channelDropGapAnchor(
        source, targetCategoryItem, targetChannelId, afterTarget, gapAfter);
    updateDragVisuals(source, anchor, gapAfter);
    return true;
}

void ChannelTree::dragMoveEvent(QDragMoveEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPoint pos = event->position().toPoint();
#else
    const QPoint pos = event->pos();
#endif
    if (updateDragTargetAt(pos)) {
        event->setDropAction(Qt::MoveAction);
        event->accept();
    } else {
        event->ignore();
    }
}

void ChannelTree::dropEvent(QDropEvent* event)
{
    QTreeWidgetItem* source = nullptr;
    if (!dragSourceIndexes.isEmpty() && dragSourceIndexes.front().isValid()) {
        source = itemFromIndex(dragSourceIndexes.front());
    }
    if (!source) {
        const auto selected = selectedItems();
        source = selected.size() == 1 ? selected.front() : currentItem();
    }
    const QPoint pos = dropEventPosition(event);

    if (source && source->data(0, ItemKindRole).toInt() == CategoryItemKind) {
        QTreeWidgetItem* targetCategoryItem = nullptr;
        bool afterTarget = false;
        if (!resolveCategoryDropTarget(source, pos, targetCategoryItem, afterTarget)) {
            resetDragVisuals(false);
            event->ignore();
            return;
        }

        const QString targetCategoryId = targetCategoryItem->data(0, ItemIdRole).toString();
        if (!categoryDropChangesPosition(source, targetCategoryItem, afterTarget)) {
            resetDragVisuals(false);
            event->setDropAction(Qt::MoveAction);
            event->accept();
            return;
        }
        resetDragVisuals(false);
        moveCategory(source, targetCategoryId, afterTarget);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }

    QTreeWidgetItem* targetCategoryItem = nullptr;
    QString targetChannelId;
    bool afterTarget = false;
    if (!resolveChannelDropTarget(source, pos, targetCategoryItem,
                                  targetChannelId, afterTarget)) {
        resetDragVisuals(false);
        event->ignore();
        return;
    }

    auto* channelItem = static_cast<ChannelItem*>(source);
    const QString targetCategoryId = targetCategoryItem->data(0, ItemIdRole).toString();
    if (!channelDropChangesPosition(source, targetCategoryItem,
                                    targetChannelId, afterTarget)) {
        resetDragVisuals(false);
        event->setDropAction(Qt::MoveAction);
        event->accept();
        return;
    }
    resetDragVisuals(false);
    moveChannel(channelItem, targetCategoryId, targetChannelId,
                afterTarget, !targetChannelId.isEmpty());
    event->setDropAction(Qt::MoveAction);
    event->accept();
}


} // namespace Mattermost
