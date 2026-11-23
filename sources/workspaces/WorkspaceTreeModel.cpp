#include "WorkspaceTreeModel.h"

#include <functional>
#include <utility>

#include "AccountListModel.h"
#include "WorkspaceLayout.h"

namespace Mattermost {

WorkspaceTreeModel::WorkspaceTreeModel(const WorkspaceLayout& layout,
                                       const AccountListModel& accounts,
                                       QObject* parent)
    : QAbstractItemModel(parent)
    , _layout(layout)
    , _accounts(accounts)
{
    refreshTopology();
    connect(&_accounts, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex& first, const QModelIndex& last,
                   const QVector<int>& roles) {
        if (!roles.isEmpty() && !roles.contains(AccountListModel::StatusRole)
            && !roles.contains(AccountListModel::ConnectionStateRole))
            return;
        for (int row = first.row(); row <= last.row(); ++row)
            notifyAccountStatus(_accounts.idAt(row));
    });
}

WorkspaceTreeModel::Node* WorkspaceTreeModel::append(
    Node* owner, NodeKind kind, const QString& id, const QString& name,
    const QString& profileId, const QString& viewId, const QString& accountId)
{
    auto node = std::make_unique<Node>();
    node->kind = kind;
    node->id = id;
    node->name = name;
    node->profileId = profileId;
    node->viewId = viewId;
    node->accountId = accountId;
    node->parent = owner;
    node->row = int(owner->children.size());
    Node* result = node.get();
    owner->children.push_back(std::move(node));
    return result;
}

void WorkspaceTreeModel::refreshTopology()
{
    // Reconcile stable nodes instead of resetting the entire tree. A server
    // alias edit must emit only dataChanged; moving a server in one view must
    // preserve indexes/expanded rows everywhere else.
    struct Desired {
        NodeKind kind;
        QString id;
        QString name;
        QString profileId;
        QString viewId;
        QString accountId;
        std::vector<Desired> children;
        QString status;
    };
    std::vector<Desired> profiles;
    for (const PendingServer& pending : _pendingServers)
        profiles.push_back({PendingNode, pending.id, pending.server,
                            {}, {}, {}, {}, pending.status});
    for (const auto& profile : _layout.profiles) {
        const WorkspaceLayout::View* soleView = nullptr;
        int viewCount = 0;
        for (const auto& view : _layout.views) {
            if (view.profileId != profile.id)
                continue;
            ++viewCount;
            soleView = &view;
        }
        // One server and one View do not need two wrapper rows. Keep the
        // persisted profile and View identities for navigation and editing.
        if (viewCount == 1 && soleView && profile.accountIds.size() == 1
            && soleView->accountIds == profile.accountIds) {
            const QString& accountId = profile.accountIds.front();
            if (const auto* account = _layout.account(accountId)) {
                profiles.push_back({AccountNode, accountId, account->name,
                    profile.id, soleView->id, accountId, {}, {}});
                continue;
            }
        }
        Desired p { ProfileNode, profile.id, profile.name,
                    profile.id, {}, {}, {}, {} };
        for (const auto& view : _layout.views) {
            if (view.profileId != profile.id) continue;
            Desired v { ViewNode, view.id, view.name,
                        profile.id, view.id, {}, {}, {} };
            for (const QString& accountId : view.accountIds) {
                const auto* account = _layout.account(accountId);
                if (!account) continue;
                v.children.push_back({ AccountNode,
                    view.id + QLatin1Char('/') + accountId,
                    account->name, profile.id, view.id, accountId, {}, {} });
            }
            p.children.push_back(std::move(v));
        }
        profiles.push_back(std::move(p));
    }

    const auto setRows = [](Node* owner, size_t start) {
        for (size_t i = start; i < owner->children.size(); ++i)
            owner->children[i]->row = int(i);
    };
    std::function<void(Node*, const std::vector<Desired>&)> reconcile;
    reconcile = [this, &reconcile, &setRows](
        Node* owner, const std::vector<Desired>& desired) {
        const QModelIndex parentIndex = modelIndex(owner);
        for (size_t row = 0; row < desired.size(); ++row) {
            const Desired& want = desired[row];
            size_t found = row;
            while (found < owner->children.size()) {
                const Node* node = owner->children[found].get();
                if (node->kind == want.kind && node->id == want.id) break;
                ++found;
            }
            if (found == owner->children.size()) {
                beginInsertRows(parentIndex, int(row), int(row));
                auto node = std::make_unique<Node>();
                node->kind = want.kind;
                node->id = want.id;
                // An inserted index must already expose its complete data
                // at endInsertRows(). Emitting dataChanged for a newly added
                // row is redundant and causes extra sidebar repaints.
                node->name = want.name;
                node->profileId = want.profileId;
                node->viewId = want.viewId;
                node->accountId = want.accountId;
                node->status = want.status;
                node->parent = owner;
                node->row = int(row);
                owner->children.insert(owner->children.begin() + row,
                                       std::move(node));
                setRows(owner, row);
                endInsertRows();
            } else if (found != row) {
                // The processed prefix is already correct; only a move toward
                // the beginning can occur, so destinationChild is row.
                beginMoveRows(parentIndex, int(found), int(found),
                              parentIndex, int(row));
                auto node = std::move(owner->children[found]);
                owner->children.erase(owner->children.begin() + found);
                owner->children.insert(owner->children.begin() + row,
                                       std::move(node));
                setRows(owner, row);
                endMoveRows();
            }
            Node* node = owner->children[row].get();
            if (node->name != want.name || node->profileId != want.profileId
                || node->viewId != want.viewId
                || node->accountId != want.accountId || node->status != want.status) {
                node->name = want.name;
                node->profileId = want.profileId;
                node->viewId = want.viewId;
                node->accountId = want.accountId;
                node->status = want.status;
                const QModelIndex idx = modelIndex(node);
                emit dataChanged(idx, idx,
                                 {Qt::DisplayRole, Qt::ToolTipRole, ProfileIdRole,
                                  ViewIdRole, AccountIdRole, StatusRole});
            }
            reconcile(node, want.children);
        }
        while (owner->children.size() > desired.size()) {
            const int last = int(owner->children.size() - 1);
            beginRemoveRows(parentIndex, last, last);
            owner->children.pop_back();
            endRemoveRows();
        }
    };
    reconcile(&_root, profiles);

    _views.clear();
    _accountOccurrences.clear();
    for (const auto& profile : _root.children) {
        if (profile->kind == AccountNode) {
            _views.insert(profile->viewId, profile.get());
            _accountOccurrences[profile->accountId].append(profile.get());
            continue;
        }
        if (profile->kind != ProfileNode) continue;
        for (const auto& view : profile->children) {
            _views.insert(view->viewId, view.get());
            for (const auto& account : view->children)
                _accountOccurrences[account->accountId].append(account.get());
        }
    }
    const QString previous = _activeViewId;
    _activeViewId = _layout.activeViewId;
    if (previous != _activeViewId) {
        if (Node* oldView = _views.value(previous, nullptr)) {
            const QModelIndex idx = modelIndex(oldView);
            emit dataChanged(idx, idx, {ActiveRole});
        }
        if (Node* newView = _views.value(_activeViewId, nullptr)) {
            const QModelIndex idx = modelIndex(newView);
            emit dataChanged(idx, idx, {ActiveRole});
        }
        emit activeViewChanged(_activeViewId);
    }
}

void WorkspaceTreeModel::setPendingServer(const QString& id,
                                           const QString& server,
                                           const QString& status)
{
    if (id.isEmpty()) return;
    for (PendingServer& pending : _pendingServers) {
        if (pending.id != id) continue;
        if (pending.server == server && pending.status == status) return;
        pending.server = server;
        pending.status = status;
        refreshTopology();
        return;
    }
    _pendingServers.append({id, server, status});
    refreshTopology();
}

void WorkspaceTreeModel::removePendingServer(const QString& id)
{
    for (int i = 0; i < _pendingServers.size(); ++i) {
        if (_pendingServers[i].id != id) continue;
        _pendingServers.removeAt(i);
        refreshTopology();
        return;
    }
}

void WorkspaceTreeModel::syncActiveView()
{
    if (_activeViewId == _layout.activeViewId)
        return;
    const QString previous = _activeViewId;
    _activeViewId = _layout.activeViewId;
    if (Node* oldView = _views.value(previous, nullptr)) {
        const QModelIndex idx = modelIndex(oldView);
        emit dataChanged(idx, idx, {ActiveRole});
    }
    if (Node* newView = _views.value(_activeViewId, nullptr)) {
        const QModelIndex idx = modelIndex(newView);
        emit dataChanged(idx, idx, {ActiveRole});
    }
    emit activeViewChanged(_activeViewId);
}

QModelIndex WorkspaceTreeModel::index(int row, int column,
                                      const QModelIndex& parentIndex) const
{
    if (column != 0 || row < 0 || (parentIndex.isValid()
                                    && parentIndex.column() != 0))
        return {};
    const Node* parentNode = parentIndex.isValid()
        ? static_cast<const Node*>(parentIndex.internalPointer()) : &_root;
    if (size_t(row) >= parentNode->children.size())
        return {};
    return createIndex(row, column, parentNode->children[size_t(row)].get());
}

QModelIndex WorkspaceTreeModel::parent(const QModelIndex& child) const
{
    if (!child.isValid())
        return {};
    const auto* node = static_cast<const Node*>(child.internalPointer());
    return modelIndex(node->parent);
}

QModelIndex WorkspaceTreeModel::modelIndex(const Node* node) const
{
    if (!node || node == &_root)
        return {};
    return createIndex(node->row, 0, const_cast<Node*>(node));
}

int WorkspaceTreeModel::rowCount(const QModelIndex& parentIndex) const
{
    if (parentIndex.isValid() && parentIndex.column() != 0)
        return 0;
    const Node* node = parentIndex.isValid()
        ? static_cast<const Node*>(parentIndex.internalPointer()) : &_root;
    return int(node->children.size());
}

int WorkspaceTreeModel::columnCount(const QModelIndex&) const
{
    return 1;
}

QVariant WorkspaceTreeModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.column() != 0)
        return {};
    const auto* node = static_cast<const Node*>(idx.internalPointer());
    switch (role) {
    case Qt::DisplayRole:
        return node->name;
    case Qt::ToolTipRole: {
        if (node->kind != AccountNode && node->kind != PendingNode)
            return {};
        const QString status = data(idx, StatusRole).toString();
        return status.isEmpty() ? QVariant(node->name)
            : QVariant(QStringLiteral("%1 — %2").arg(node->name, status));
    }
    case NodeKindRole: return static_cast<int>(node->kind);
    case NodeIdRole: return node->id;
    case ProfileIdRole: return node->profileId;
    case ViewIdRole: return node->viewId;
    case AccountIdRole: return node->accountId;
    case ActiveRole:
        return (node->kind == ViewNode
                || (node->kind == AccountNode && node->parent == &_root))
            && node->viewId == _activeViewId;
    case StatusRole:
    case ConnectionStateRole: {
        if (node->kind == PendingNode)
            return role == StatusRole ? QVariant(node->status)
                                      : QVariant();
        if (node->kind != AccountNode)
            return {};
        const int row = _accounts.rowForId(node->accountId);
        if (row < 0)
            return {};
        return _accounts.data(_accounts.index(row, 0),
            role == StatusRole ? AccountListModel::StatusRole
                               : AccountListModel::ConnectionStateRole);
    }
    default: return {};
    }
}

QHash<int, QByteArray> WorkspaceTreeModel::roleNames() const
{
    return {
        {NodeKindRole, QByteArrayLiteral("nodeKind")},
        {NodeIdRole, QByteArrayLiteral("nodeId")},
        {ProfileIdRole, QByteArrayLiteral("profileId")},
        {ViewIdRole, QByteArrayLiteral("viewId")},
        {AccountIdRole, QByteArrayLiteral("accountId")},
        {ConnectionStateRole, QByteArrayLiteral("connectionState")},
        {StatusRole, QByteArrayLiteral("status")},
        {ActiveRole, QByteArrayLiteral("active")}
    };
}

void WorkspaceTreeModel::notifyAccountStatus(const QString& accountId)
{
    for (Node* node : _accountOccurrences.value(accountId)) {
        const QModelIndex idx = modelIndex(node);
        emit dataChanged(idx, idx,
                         {ConnectionStateRole, StatusRole, Qt::ToolTipRole});
    }
}

} // namespace Mattermost
