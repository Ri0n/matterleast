#include "ViewConversationModel.h"
#include <algorithm>
#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendDirectChannelsTeam.h"

namespace Mattermost {

ViewConversationModel::ViewConversationModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ViewConversationModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : _rows.size();
}

QVariant ViewConversationModel::data(const QModelIndex& idx, int role) const
{
    if (!idx.isValid() || idx.column() != 0
        || idx.row() < 0 || idx.row() >= _rows.size())
        return {};
    const Entry& entry = _rows.at(idx.row());
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole: return entry.title;
    case AccountIdRole: return entry.accountId;
    case ChannelIdRole: return entry.channelId;
    case ActivityRole: return QVariant::fromValue(entry.activityAt);
    default: return {};
    }
}

QHash<int, QByteArray> ViewConversationModel::roleNames() const
{
    return {
        {AccountIdRole, QByteArrayLiteral("accountId")},
        {ChannelIdRole, QByteArrayLiteral("channelId")},
        {TitleRole, QByteArrayLiteral("title")},
        {ActivityRole, QByteArrayLiteral("activityAt")}
    };
}

bool ViewConversationModel::setSorting(const QString& sorting)
{
    if (sorting != QLatin1String("recent") && sorting != QLatin1String("alpha"))
        return false;
    if (_sorting == sorting)
        return true;
    _sorting = sorting;
    emit sortingChanged();
    refresh();
    return true;
}

void ViewConversationModel::setSources(const QVector<Source>& sources)
{
    bool unchanged = _sources.size() == sources.size();
    if (unchanged) {
        for (int i = 0; i < sources.size(); ++i) {
            if (_sources.at(i).accountId != sources.at(i).accountId
                || _sources.at(i).backend.data() != sources.at(i).backend.data()) {
                unchanged = false;
                break;
            }
        }
    }
    if (unchanged)
        return;

    for (const auto& connection : _connections)
        disconnect(connection);
    _connections.clear();
    _observedSidebars.clear();
    _sources = sources;
    for (const Source& source : _sources) {
        Backend* backend = source.backend.data();
        if (!backend)
            continue;
        _connections.push_back(connect(
            backend, &Backend::onAllTeamChannelsPopulated,
            this, &ViewConversationModel::refresh));
        // No eager SidebarService::instance() here: its constructor starts
        // preference/config network requests, even for hidden Views.
        auto& storage = backend->getStorage();
        _connections.push_back(connect(
            &storage.directChannels, &BackendDirectChannelsTeam::onNewChannel,
            this, &ViewConversationModel::refresh));
        _connections.push_back(connect(
            &storage.groupChannels, &BackendDirectChannelsTeam::onNewChannel,
            this, &ViewConversationModel::refresh));
    }
    refresh();
}

ViewConversationModel::Entry ViewConversationModel::entryAt(int row) const
{
    return row >= 0 && row < _rows.size() ? _rows.at(row) : Entry {};
}

int ViewConversationModel::rowFor(const QString& accountId,
                                   const QString& channelId) const
{
    for (int row = 0; row < _rows.size(); ++row) {
        const Entry& entry = _rows.at(row);
        if (entry.accountId == accountId && entry.channelId == channelId)
            return row;
    }
    return -1;
}

void ViewConversationModel::refresh()
{
    QVector<Entry> desired;
    if (_sources.size() >= 2) {
        for (const Source& source : _sources) {
            Backend* backend = source.backend.data();
            if (!backend || source.accountId.isEmpty())
                continue;
            SidebarService* sidebar = SidebarService::existing(*backend);
            if (!sidebar)
                continue;
            if (_observedSidebars.value(backend).data() != sidebar) {
                _observedSidebars.insert(backend, sidebar);
                _connections.push_back(connect(
                    sidebar, &SidebarService::channelActivityChanged,
                    this, &ViewConversationModel::refresh));
                _connections.push_back(connect(
                    sidebar, &SidebarService::channelActivityReset,
                    this, &ViewConversationModel::refresh));
                _connections.push_back(connect(
                    sidebar, &SidebarService::categoriesChanged,
                    this, &ViewConversationModel::refresh));
            }
            auto& storage = backend->getStorage();
            for (const QString& channelId : sidebar->visibleDirectConversationIds()) {
                const BackendChannel* channel = storage.getChannelById(channelId);
                if (!channel)
                    continue;
                const quint64 activity = std::max<quint64>(
                    std::max<quint64>(channel->create_at, channel->last_post_at),
                    sidebar->channelActivityTime(*channel));
                desired.push_back({source.accountId, channelId,
                                   channel->display_name, activity});
            }
        }
        const bool alpha = _sorting == QLatin1String("alpha");
        std::sort(desired.begin(), desired.end(),
                  [alpha](const Entry& a, const Entry& b) {
            if (!alpha && a.activityAt != b.activityAt)
                return a.activityAt > b.activityAt;
            const int byTitle = QString::compare(a.title, b.title,
                                                  Qt::CaseInsensitive);
            if (byTitle != 0)
                return byTitle < 0;
            const int byAccount = QString::compare(a.accountId, b.accountId);
            if (byAccount != 0)
                return byAccount < 0;
            return a.channelId < b.channelId;
        });
    }
    reconcile(desired);
}

void ViewConversationModel::reconcile(const QVector<Entry>& desired)
{
    // Emit only incremental changes; preserve indexes and QML delegates.
    for (int row = 0; row < desired.size(); ++row) {
        const Entry& want = desired.at(row);
        int found = row;
        while (found < _rows.size()
               && (_rows.at(found).accountId != want.accountId
                   || _rows.at(found).channelId != want.channelId))
            ++found;
        if (found == _rows.size()) {
            beginInsertRows({}, row, row);
            _rows.insert(row, want);
            endInsertRows();
        } else {
            if (found != row) {
                beginMoveRows({}, found, found, {}, row);
                const Entry moved = _rows.takeAt(found);
                _rows.insert(row, moved);
                endMoveRows();
            }
            Entry& current = _rows[row];
            if (current.title != want.title || current.activityAt != want.activityAt) {
                current = want;
                const QModelIndex idx = index(row, 0);
                emit dataChanged(idx, idx, {Qt::DisplayRole, TitleRole, ActivityRole});
            }
        }
    }
    if (_rows.size() > desired.size()) {
        beginRemoveRows({}, desired.size(), _rows.size() - 1);
        _rows.resize(desired.size());
        endRemoveRows();
    }
}

} // namespace Mattermost
