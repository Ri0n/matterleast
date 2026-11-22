#include "ViewChannelsModel.h"

#include <algorithm>
#include <utility>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendTeam.h"
#include "workspaces/WorkspaceLayout.h"

namespace Mattermost {

ViewChannelsModel::ViewChannelsModel(const WorkspaceLayout& layout, QObject* parent)
    : QAbstractItemModel(parent)
    , _layout(layout)
{
    syncView();
}

ViewChannelsModel::Desired ViewChannelsModel::describeCategory(
    const QString& accountId, const QString& teamId,
    const SidebarCategory& category, Backend& backend,
    const SidebarService& sidebar) const
{
    Desired result;
    result.kind = CategoryNode;
    result.id = category.id;
    result.name = category.displayName;
    result.accountId = accountId;
    result.teamId = teamId;
    result.categoryId = category.id;
    result.collapsed = category.collapsed;

    // SidebarService is authoritative for per-server category ordering,
    // DM/GM visibility limits, muted conversations and unread prioritization.
    // Do not let an unrelated account's search filter change this projection.
    const QStringList visible = sidebar.visibleChannelIds(category, false);
    result.children.reserve(size_t(visible.size()));
    for (const QString& channelId : visible) {
        BackendChannel* channel = backend.getStorage().getChannelById(channelId);
        if (!channel)
            continue;
        Desired item;
        item.kind = ChannelNode;
        item.id = channelId;
        item.name = channel->display_name.isEmpty()
            ? channel->name : channel->display_name;
        item.accountId = accountId;
        item.teamId = teamId;
        item.categoryId = category.id;
        item.channelId = channelId;
        item.connected = true;
        item.unread = sidebar.isChannelUnread(*channel);
        item.mentioned = sidebar.hasUnreadMention(channelId);
        item.muted = sidebar.isChannelMuted(*channel);
        item.activityAt = sidebar.channelActivityTime(*channel);
        result.children.push_back(std::move(item));
    }
    return result;
}

ViewChannelsModel::Desired
ViewChannelsModel::describeAccount(const QString& accountId) const
{
    Desired account;
    account.kind = AccountNode;
    account.id = accountId;
    account.accountId = accountId;
    if (const auto* record = _layout.account(accountId))
        account.name = record->name;

    Backend* backend = _backends.value(accountId).data();
    account.connected = backend != nullptr;
    if (!backend)
        return account;

    const auto& storage = backend->getStorage();
    const auto& sidebar = SidebarService::instance(*backend);
    account.children.reserve(storage.teams.size());
    for (const auto& entry : storage.teams) {
        const BackendTeam& team = entry.second;
        Desired teamNode;
        teamNode.kind = TeamNode;
        teamNode.id = team.id;
        teamNode.accountId = accountId;
        teamNode.teamId = team.id;
        teamNode.name = team.display_name.isEmpty()
            ? team.name : team.display_name;
        teamNode.connected = true;
        if (const SidebarTeamState* state = sidebar.teamState(team.id)) {
            teamNode.children.reserve(size_t(state->order.size()));
            for (const QString& categoryId : state->order) {
                const SidebarCategory* category = state->category(categoryId);
                if (category)
                    teamNode.children.push_back(describeCategory(
                        accountId, team.id, *category, *backend, sidebar));
            }
        }
        account.children.push_back(std::move(teamNode));
    }
    return account;
}

QModelIndex ViewChannelsModel::modelIndex(const Node* node) const
{
    if (!node || node == &_root)
        return {};
    return createIndex(node->row, 0, const_cast<Node*>(node));
}

void ViewChannelsModel::updateData(Node* node, const Desired& want)
{
    if (node->name == want.name && node->connected == want.connected
        && node->unread == want.unread && node->mentioned == want.mentioned
        && node->muted == want.muted && node->collapsed == want.collapsed
        && node->activityAt == want.activityAt)
        return;

    QVector<int> roles;
    if (node->name != want.name) roles.append(Qt::DisplayRole);
    if (node->connected != want.connected) roles.append(ConnectedRole);
    if (node->unread != want.unread) roles.append(UnreadRole);
    if (node->mentioned != want.mentioned) roles.append(MentionedRole);
    if (node->muted != want.muted) roles.append(MutedRole);
    if (node->collapsed != want.collapsed) roles.append(CollapsedRole);
    if (node->activityAt != want.activityAt) roles.append(ActivityAtRole);
    node->name = want.name;
    node->connected = want.connected;
    node->unread = want.unread;
    node->mentioned = want.mentioned;
    node->muted = want.muted;
    node->collapsed = want.collapsed;
    node->activityAt = want.activityAt;
    emit dataChanged(modelIndex(node), modelIndex(node), roles);
}

void ViewChannelsModel::reconcile(
    Node* owner, const std::vector<Desired>& desired)
{
    const QModelIndex parentIndex = modelIndex(owner);
    const auto updateRows = [](Node* parent, size_t first) {
        for (size_t row = first; row < parent->children.size(); ++row)
            parent->children[row]->row = int(row);
    };

    for (size_t row = 0; row < desired.size(); ++row) {
        const Desired& want = desired[row];
        size_t found = row;
        while (found < owner->children.size()
               && (owner->children[found]->kind != want.kind
                   || owner->children[found]->id != want.id))
            ++found;

        if (found == owner->children.size()) {
            beginInsertRows(parentIndex, int(row), int(row));
            auto node = std::make_unique<Node>();
            node->kind = want.kind;
            node->id = want.id;
            node->name = want.name;
            node->accountId = want.accountId;
            node->teamId = want.teamId;
            node->categoryId = want.categoryId;
            node->channelId = want.channelId;
            node->connected = want.connected;
            node->unread = want.unread;
            node->mentioned = want.mentioned;
            node->muted = want.muted;
            node->collapsed = want.collapsed;
            node->activityAt = want.activityAt;
            node->owner = owner;
            node->row = int(row);
            owner->children.insert(owner->children.begin() + row,
                                   std::move(node));
            updateRows(owner, row);
            endInsertRows();
        } else if (found != row) {
            // Processed prefix is fixed, so every move goes toward row zero.
            beginMoveRows(parentIndex, int(found), int(found),
                          parentIndex, int(row));
            auto node = std::move(owner->children[found]);
            owner->children.erase(owner->children.begin() + found);
            owner->children.insert(owner->children.begin() + row,
                                   std::move(node));
            updateRows(owner, row);
            endMoveRows();
        }

        Node* node = owner->children[row].get();
        updateData(node, want);
        reconcile(node, want.children);
    }
    while (owner->children.size() > desired.size()) {
        const int last = int(owner->children.size() - 1);
        beginRemoveRows(parentIndex, last, last);
        owner->children.pop_back();
        endRemoveRows();
    }
}

void ViewChannelsModel::syncView()
{
    const bool changed = _activeViewId != _layout.activeViewId;
    // Inserted indexes must already expose the destination view's identity
    // when Qt delivers rowsInserted, not the outgoing view's identity.
    _activeViewId = _layout.activeViewId;
    std::vector<Desired> desired;
    for (const auto& view : _layout.views) {
        if (view.id != _layout.activeViewId)
            continue;
        desired.reserve(size_t(view.accountIds.size()));
        for (const QString& id : view.accountIds)
            if (_layout.account(id))
                desired.push_back(describeAccount(id));
        break;
    }
    reconcile(&_root, desired);
    _accountNodes.clear();
    for (const auto& node : _root.children)
        _accountNodes.insert(node->accountId, node.get());

    if (changed)
        emit activeViewChanged(_activeViewId);
}

void ViewChannelsModel::refreshAccount(const QString& accountId)
{
    Node* node = _accountNodes.value(accountId, nullptr);
    if (!node)
        return;
    const Desired description = describeAccount(accountId);
    updateData(node, description);
    reconcile(node, description.children);
}

void ViewChannelsModel::refreshChannelActivity(
    const QString& accountId, const QString& channelId)
{
    Node* account = _accountNodes.value(accountId, nullptr);
    Backend* backend = _backends.value(accountId).data();
    if (!account || !backend || channelId.isEmpty())
        return;

    const auto& sidebar = SidebarService::instance(*backend);
    // A post can alter a DM/GM's inclusion, unread badge or "recent" order.
    // Reconcile only the categories containing that conversation, not every
    // server, team or channel in the active view.
    for (const auto& team : account->children) {
        const SidebarTeamState* state = sidebar.teamState(team->teamId);
        if (!state)
            continue;
        for (const auto& category : team->children) {
            const SidebarCategory* source = state->category(category->categoryId);
            if (!source || !source->channelIds.contains(channelId))
                continue;
            const Desired next = describeCategory(
                accountId, team->teamId, *source, *backend, sidebar);
            updateData(category.get(), next);
            reconcile(category.get(), next.children);
        }
    }
}

void ViewChannelsModel::setBackend(const QString& accountId, Backend* backend)
{
    if (accountId.isEmpty() || !_layout.account(accountId))
        return;
    if (_backends.value(accountId).data() == backend)
        return;

    for (const QMetaObject::Connection& connection : _connections.take(accountId))
        QObject::disconnect(connection);
    if (!backend) {
        _backends.remove(accountId);
        refreshAccount(accountId);
        return;
    }

    _backends.insert(accountId, backend);
    auto& connections = _connections[accountId];
    auto& sidebar = SidebarService::instance(*backend);
    connections.append(connect(&sidebar, &SidebarService::categoriesChanged,
                               this, [this, accountId](const QString&) {
        refreshAccount(accountId);
    }));
    connections.append(connect(&sidebar, &SidebarService::channelActivityChanged,
                               this, [this, accountId](const QString& channelId) {
        refreshChannelActivity(accountId, channelId);
    }));
    connections.append(connect(&sidebar, &SidebarService::channelActivityReset,
                               this, [this, accountId] {
        refreshAccount(accountId);
    }));
    connections.append(connect(&sidebar, &SidebarService::channelMutedChanged,
                               this, [this, accountId](const QString& channelId, bool) {
        refreshChannelActivity(accountId, channelId);
    }));
    connections.append(connect(backend, &Backend::onAllTeamChannelsPopulated,
                               this, [this, accountId] {
        refreshAccount(accountId);
    }));
    connections.append(connect(backend, &Backend::onAddedToTeam,
                               this, [this, accountId](BackendTeam&) {
        refreshAccount(accountId);
    }));
    connections.append(connect(backend, &QObject::destroyed,
                               this, [this, accountId] {
        // The durable row remains, but no stale channel identity survives a
        // disposed Backend/Storage. New Backend instances bind separately.
        _backends.remove(accountId);
        _connections.remove(accountId);
        refreshAccount(accountId);
    }));
    refreshAccount(accountId);
}

QModelIndex ViewChannelsModel::index(int row, int column,
                                      const QModelIndex& parentIndex) const
{
    if (column != 0 || row < 0
        || (parentIndex.isValid() && parentIndex.column() != 0))
        return {};
    const auto* owner = parentIndex.isValid()
        ? static_cast<const Node*>(parentIndex.internalPointer()) : &_root;
    if (size_t(row) >= owner->children.size())
        return {};
    return createIndex(row, column, owner->children[size_t(row)].get());
}

QModelIndex ViewChannelsModel::parent(const QModelIndex& child) const
{
    if (!child.isValid())
        return {};
    const auto* node = static_cast<const Node*>(child.internalPointer());
    return modelIndex(node->owner);
}

int ViewChannelsModel::rowCount(const QModelIndex& parentIndex) const
{
    if (parentIndex.isValid() && parentIndex.column() != 0)
        return 0;
    const auto* node = parentIndex.isValid()
        ? static_cast<const Node*>(parentIndex.internalPointer()) : &_root;
    return int(node->children.size());
}

int ViewChannelsModel::columnCount(const QModelIndex&) const
{
    return 1;
}

QVariant ViewChannelsModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.column() != 0)
        return {};
    const auto* node = static_cast<const Node*>(idx.internalPointer());
    switch (role) {
    case Qt::DisplayRole: return node->name;
    case NodeKindRole: return int(node->kind);
    case NodeIdRole:
        // Distinguish the same channel ID in another account, category or
        // view; never use a bare Mattermost channel ID as a global key.
        return _activeViewId + QLatin1Char('/') + node->accountId
            + QLatin1Char('/') + node->teamId
            + QLatin1Char('/') + node->categoryId
            + QLatin1Char('/') + node->channelId;
    case AccountIdRole: return node->accountId;
    case TeamIdRole: return node->teamId;
    case CategoryIdRole: return node->categoryId;
    case ChannelIdRole: return node->channelId;
    case ConnectedRole: return node->connected;
    case UnreadRole: return node->unread;
    case MentionedRole: return node->mentioned;
    case MutedRole: return node->muted;
    case CollapsedRole: return node->collapsed;
    case ActivityAtRole: return QVariant::fromValue(qulonglong(node->activityAt));
    default: return {};
    }
}

QHash<int, QByteArray> ViewChannelsModel::roleNames() const
{
    return {
        {NodeKindRole, QByteArrayLiteral("nodeKind")},
        {NodeIdRole, QByteArrayLiteral("nodeId")},
        {AccountIdRole, QByteArrayLiteral("accountId")},
        {TeamIdRole, QByteArrayLiteral("teamId")},
        {CategoryIdRole, QByteArrayLiteral("categoryId")},
        {ChannelIdRole, QByteArrayLiteral("channelId")},
        {ConnectedRole, QByteArrayLiteral("connected")},
        {UnreadRole, QByteArrayLiteral("unread")},
        {MentionedRole, QByteArrayLiteral("mentioned")},
        {MutedRole, QByteArrayLiteral("muted")},
        {CollapsedRole, QByteArrayLiteral("collapsed")},
        {ActivityAtRole, QByteArrayLiteral("activityAt")}
    };
}

} // namespace Mattermost
