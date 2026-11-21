#include "WorkspaceTreeModel.h"

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
    beginResetModel();
    _root.children.clear();
    _views.clear();
    _accountOccurrences.clear();

    for (const WorkspaceLayout::Profile& profile : _layout.profiles) {
        Node* profileNode = append(&_root, ProfileNode, profile.id, profile.name,
                                   profile.id, {});
        for (const WorkspaceLayout::View& view : _layout.views) {
            if (view.profileId != profile.id)
                continue;
            Node* viewNode = append(profileNode, ViewNode, view.id, view.name,
                                    profile.id, view.id);
            _views.insert(view.id, viewNode);
            for (const QString& accountId : view.accountIds) {
                const auto* account = _layout.account(accountId);
                if (!account)
                    continue;
                Node* accountNode = append(viewNode, AccountNode,
                                           view.id + QLatin1Char('/') + accountId,
                                           account->name, profile.id,
                                           view.id, accountId);
                _accountOccurrences[accountId].append(accountNode);
            }
        }
    }
    const bool changed = _activeViewId != _layout.activeViewId;
    _activeViewId = _layout.activeViewId;
    endResetModel();
    if (changed)
        emit activeViewChanged(_activeViewId);
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
    case Qt::DisplayRole: return node->name;
    case NodeKindRole: return static_cast<int>(node->kind);
    case NodeIdRole: return node->id;
    case ProfileIdRole: return node->profileId;
    case ViewIdRole: return node->viewId;
    case AccountIdRole: return node->accountId;
    case ActiveRole: return node->kind == ViewNode
                            && node->viewId == _activeViewId;
    case StatusRole:
    case ConnectionStateRole: {
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
        emit dataChanged(idx, idx, {ConnectionStateRole, StatusRole});
    }
}

} // namespace Mattermost
