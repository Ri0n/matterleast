#include "ViewSidebarModel.h"

#include <QSet>

#include "ViewConversationModel.h"
#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendTeam.h"

namespace Mattermost {

ViewSidebarModel::ViewSidebarModel(QObject* parent)
    : QAbstractItemModel(parent)
{
}

QModelIndex ViewSidebarModel::modelIndex(const Node* node) const
{
    return node && node != &_root
        ? createIndex(node->row, 0, const_cast<Node*>(node)) : QModelIndex();
}

QModelIndex ViewSidebarModel::index(int row, int column,
                                   const QModelIndex& parentIndex) const
{
    if (row < 0 || column != 0 || (parentIndex.isValid() && parentIndex.column() != 0))
        return {};
    const Node* parentNode = parentIndex.isValid()
        ? static_cast<const Node*>(parentIndex.internalPointer()) : &_root;
    if (static_cast<size_t>(row) >= parentNode->children.size()) return {};
    return modelIndex(parentNode->children[static_cast<size_t>(row)].get());
}

QModelIndex ViewSidebarModel::parent(const QModelIndex& child) const
{
    if (!child.isValid()) return {};
    const auto* node = static_cast<const Node*>(child.internalPointer());
    return modelIndex(node->parent);
}

int ViewSidebarModel::rowCount(const QModelIndex& parentIndex) const
{
    if (parentIndex.isValid() && parentIndex.column() != 0) return 0;
    const Node* node = parentIndex.isValid()
        ? static_cast<const Node*>(parentIndex.internalPointer()) : &_root;
    return int(node->children.size());
}

int ViewSidebarModel::columnCount(const QModelIndex&) const
{
    return 1;
}

QVariant ViewSidebarModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.column() != 0) return {};
    const auto* node = static_cast<const Node*>(idx.internalPointer());
    switch (role) {
    case Qt::DisplayRole: return node->title;
    case KindRole: return int(node->kind);
    case AccountIdRole: return node->accountId;
    case TeamIdRole: return node->teamId;
    case CategoryIdRole: return node->categoryId;
    case ChannelIdRole: return node->channelId;
    default: return {};
    }
}

QHash<int, QByteArray> ViewSidebarModel::roleNames() const
{
    return {
        {KindRole, QByteArrayLiteral("kind")},
        {AccountIdRole, QByteArrayLiteral("accountId")},
        {TeamIdRole, QByteArrayLiteral("teamId")},
        {CategoryIdRole, QByteArrayLiteral("categoryId")},
        {ChannelIdRole, QByteArrayLiteral("channelId")}
    };
}

QModelIndex ViewSidebarModel::findChannel(const QString& accountId,
                                         const QString& channelId) const
{
    if (accountId.isEmpty() || channelId.isEmpty()) return {};
    // A channel may appear in more than one category (e.g. Favorites). Keep
    // the account identity in every row; never match by remote ID alone.
    for (const auto& root : _root.children) {
        if (root->kind == SharedDirectMessages) {
            for (const auto& entry : root->children)
                if (entry->accountId == accountId && entry->channelId == channelId)
                    return modelIndex(entry.get());
        } else if (root->accountId == accountId) {
            for (const auto& team : root->children)
                for (const auto& category : team->children)
                    for (const auto& entry : category->children)
                        if (entry->channelId == channelId)
                            return modelIndex(entry.get());
        }
    }
    return {};
}

void ViewSidebarModel::setSources(const QVector<Source>& sources,
                                  ViewConversationModel* conversations)
{
    bool unchanged = _conversations == conversations && _sources.size() == sources.size();
    for (int i = 0; unchanged && i < sources.size(); ++i) {
        const auto& previous = _sources.at(i);
        const auto& current = sources.at(i);
        if (previous.accountId != current.accountId || previous.name != current.name
            || previous.backend != current.backend)
            unchanged = false;
    }
    if (unchanged) return;

    for (const auto& connection : _connections)
        disconnect(connection);
    _connections.clear();
    _observedSidebars.clear();
    _sources = sources;
    _conversations = conversations;

    for (const Source& source : _sources) {
        Backend* backend = source.backend.data();
        if (!backend) continue;
        _connections.push_back(connect(backend, &Backend::onAllTeamChannelsPopulated,
                                       this, &ViewSidebarModel::refresh));
        _connections.push_back(connect(backend, &Backend::onAddedToTeam,
                                       this, [this](BackendTeam&) { refresh(); }));
    }
    if (_conversations) {
        auto* model = _conversations.data();
        _connections.push_back(connect(model, &QAbstractItemModel::rowsInserted,
                                       this, &ViewSidebarModel::refresh));
        _connections.push_back(connect(model, &QAbstractItemModel::rowsRemoved,
                                       this, &ViewSidebarModel::refresh));
        _connections.push_back(connect(model, &QAbstractItemModel::rowsMoved,
                                       this, &ViewSidebarModel::refresh));
        _connections.push_back(connect(model, &QAbstractItemModel::dataChanged,
                                       this, &ViewSidebarModel::refresh));
        _connections.push_back(connect(model, &QAbstractItemModel::modelReset,
                                       this, &ViewSidebarModel::refresh));
    }
    refresh();
}

void ViewSidebarModel::observeSidebar(Backend& backend, SidebarService& sidebar)
{
    if (_observedSidebars.value(&backend) == &sidebar) return;
    _observedSidebars.insert(&backend, &sidebar);
    _connections.push_back(connect(&sidebar, &SidebarService::categoriesChanged,
                                   this, &ViewSidebarModel::refresh));
    _connections.push_back(connect(&sidebar, &SidebarService::channelActivityChanged,
                                   this, &ViewSidebarModel::refresh));
    _connections.push_back(connect(&sidebar, &SidebarService::channelActivityReset,
                                   this, &ViewSidebarModel::refresh));
}

void ViewSidebarModel::refresh()
{
    std::vector<Desired> desired;
    // The singleton view has exactly the old sidebar; no wrappers or shared DM.
    if (_sources.size() < 2) {
        reconcile(&_root, desired);
        return;
    }

    for (const Source& source : _sources) {
        if (source.accountId.isEmpty()) continue;
        Desired account {Account, source.accountId,
                         source.name.isEmpty() ? source.accountId : source.name,
                         source.accountId, {}, {}, {}, {}};
        Backend* backend = source.backend.data();
        if (backend) {
            SidebarService* sidebar = SidebarService::existing(*backend);
            if (sidebar) observeSidebar(*backend, *sidebar);
            const Storage& storage = backend->getStorage();
            for (const auto& pair : storage.teams) {
                const BackendTeam& sourceTeam = pair.second;
                if (sourceTeam.delete_at) continue;
                Desired team {Team, sourceTeam.id,
                    sourceTeam.display_name.isEmpty() ? sourceTeam.name
                        : sourceTeam.display_name,
                    source.accountId, sourceTeam.id, {}, {}, {}};
                if (team.title.isEmpty()) team.title = sourceTeam.id;
                const SidebarTeamState* state = sidebar
                    ? sidebar->teamState(sourceTeam.id) : nullptr;
                if (state) {
                    for (const QString& categoryId : state->order) {
                        const SidebarCategory* sourceCategory = state->category(categoryId);
                        if (!sourceCategory
                            || sourceCategory->type == QLatin1String("direct_messages"))
                            continue;
                        Desired category {Category, sourceCategory->id,
                            sourceCategory->displayName.isEmpty()
                                ? sourceCategory->type : sourceCategory->displayName,
                            source.accountId, sourceTeam.id, sourceCategory->id, {}, {}};
                        QSet<QString> seenChannels;
                        for (const QString& channelId : sidebar->visibleChannelIds(*sourceCategory)) {
                            if (seenChannels.contains(channelId)) continue;
                            seenChannels.insert(channelId);
                            const BackendChannel* channel = storage.getChannelById(channelId);
                            if (!channel || channel->type == BackendChannel::directChannel
                                || channel->type == BackendChannel::groupChannel
                                || channel->team != &sourceTeam)
                                continue;
                            category.children.push_back({Channel, channelId,
                                channel->display_name.isEmpty() ? channel->name
                                    : channel->display_name,
                                source.accountId, sourceTeam.id,
                                sourceCategory->id, channelId, {}});
                        }
                        team.children.push_back(std::move(category));
                    }
                }
                account.children.push_back(std::move(team));
            }
        }
        desired.push_back(std::move(account));
    }

    Desired shared {SharedDirectMessages, QStringLiteral("shared-direct"),
                    tr("Direct Messages"), {}, {}, {}, {}, {}};
    if (_conversations) {
        for (int i = 0; i < _conversations->rowCount(); ++i) {
            const auto entry = _conversations->entryAt(i);
            if (entry.accountId.isEmpty() || entry.channelId.isEmpty()) continue;
            shared.children.push_back({Channel, entry.accountId
                + QLatin1Char('/') + entry.channelId, entry.title,
                entry.accountId, {}, {}, entry.channelId, {}});
        }
    }
    desired.push_back(std::move(shared));
    reconcile(&_root, desired);
}

void ViewSidebarModel::reconcile(Node* owner, const std::vector<Desired>& desired)
{
    const QModelIndex parentIndex = modelIndex(owner);
    const auto updateRows = [owner](size_t start) {
        for (size_t i = start; i < owner->children.size(); ++i)
            owner->children[i]->row = int(i);
    };
    for (size_t row = 0; row < desired.size(); ++row) {
        const Desired& want = desired[row];
        size_t found = row;
        while (found < owner->children.size()) {
            const Node& candidate = *owner->children[found];
            if (candidate.kind == want.kind && candidate.id == want.id) break;
            ++found;
        }
        if (found == owner->children.size()) {
            beginInsertRows(parentIndex, int(row), int(row));
            auto node = std::make_unique<Node>();
            node->kind = want.kind;
            node->id = want.id;
            node->title = want.title;
            node->accountId = want.accountId;
            node->teamId = want.teamId;
            node->categoryId = want.categoryId;
            node->channelId = want.channelId;
            node->parent = owner;
            owner->children.insert(owner->children.begin() + row, std::move(node));
            updateRows(row);
            endInsertRows();
        } else if (found != row) {
            beginMoveRows(parentIndex, int(found), int(found), parentIndex, int(row));
            auto node = std::move(owner->children[found]);
            owner->children.erase(owner->children.begin() + found);
            owner->children.insert(owner->children.begin() + row, std::move(node));
            updateRows(row);
            endMoveRows();
        }
        Node* node = owner->children[row].get();
        if (node->title != want.title || node->accountId != want.accountId
            || node->teamId != want.teamId || node->categoryId != want.categoryId
            || node->channelId != want.channelId) {
            node->title = want.title;
            node->accountId = want.accountId;
            node->teamId = want.teamId;
            node->categoryId = want.categoryId;
            node->channelId = want.channelId;
            const QModelIndex idx = modelIndex(node);
            emit dataChanged(idx, idx, {Qt::DisplayRole, AccountIdRole,
                                        TeamIdRole, CategoryIdRole, ChannelIdRole});
        }
        reconcile(node, want.children);
    }
    while (owner->children.size() > desired.size()) {
        const int last = int(owner->children.size()) - 1;
        beginRemoveRows(parentIndex, last, last);
        owner->children.pop_back();
        endRemoveRows();
    }
}

} // namespace Mattermost
