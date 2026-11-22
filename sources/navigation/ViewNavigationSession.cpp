#include "ViewNavigationSession.h"
#include <utility>

namespace Mattermost {

ViewNavigationSession::ViewNavigationSession(QString viewId, QObject* parent)
    : QAbstractListModel(parent), _viewId(std::move(viewId))
{
}

int ViewNavigationSession::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : _tabs.count();
}

QVariant ViewNavigationSession::data(const QModelIndex& idx, int role) const
{
    const auto* entry = idx.isValid() && idx.column() == 0
        ? _tabs.at(idx.row()) : nullptr;
    if (!entry) return {};
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole: return entry->title;
    case AccountIdRole: return entry->accountId;
    case ChannelIdRole: return entry->channelId;
    case RootIdRole: return entry->rootId;
    case PostIdRole: return entry->postId;
    case PinnedRole: return entry->pinned;
    case ActiveRole: return idx.row() == _activeTab;
    default: return {};
    }
}

QHash<int, QByteArray> ViewNavigationSession::roleNames() const
{
    return {
        {AccountIdRole, QByteArrayLiteral("accountId")},
        {ChannelIdRole, QByteArrayLiteral("channelId")},
        {RootIdRole, QByteArrayLiteral("rootId")},
        {PostIdRole, QByteArrayLiteral("postId")},
        {TitleRole, QByteArrayLiteral("title")},
        {PinnedRole, QByteArrayLiteral("pinned")},
        {ActiveRole, QByteArrayLiteral("active")}
    };
}

void ViewNavigationSession::pushHistory(
    QVector<Location>& history, const Location& location)
{
    if (!location.valid()) return;
    if (history.isEmpty() || !history.last().sameDestination(location))
        history.append(location);
    constexpr int MaxHistory = 100;
    if (history.size() > MaxHistory)
        history.remove(0, history.size() - MaxHistory);
}

void ViewNavigationSession::recordLocation(const Location& location)
{
    if (_current.sameDestination(location)) {
        _current = location;
        return;
    }
    pushHistory(_back, _current);
    _forward.clear();
    _current = location;
    emit historyChanged();
}

void ViewNavigationSession::chooseTab(int index)
{
    if (_activeTab == index) return;
    const int old = _activeTab;
    _activeTab = index;
    if (old >= 0 && old < rowCount())
        emit dataChanged(this->index(old, 0), this->index(old, 0), {ActiveRole});
    if (index >= 0)
        emit dataChanged(this->index(index, 0), this->index(index, 0), {ActiveRole});
    emit activeTabChanged(index);
}

int ViewNavigationSession::openTab(
    const Location& location, const QString& title, bool pinned)
{
    if (!location.valid() || location.kind != Location::Kind::Chat)
        return -1;
    int target = _tabs.findDestination(
        location.channelId, location.rootId, location.accountId);
    if (target >= 0) {
        auto entry = *_tabs.at(target);
        if (!location.postId.isEmpty()) entry.postId = location.postId;
        if (!title.isEmpty()) entry.title = title;
        entry.pinned |= pinned;
        if (location.viewportAnchor) entry.viewportAnchor = location.viewportAnchor;
        _tabs.replace(target, std::move(entry));
        emit dataChanged(index(target, 0), index(target, 0),
                         {TitleRole, PostIdRole, PinnedRole});
    } else {
        NavigationTabsModel::Entry entry;
        entry.accountId = location.accountId;
        entry.channelId = location.channelId;
        entry.rootId = location.rootId;
        entry.postId = location.postId;
        entry.viewportAnchor = location.viewportAnchor;
        entry.title = title;
        entry.pinned = pinned;
        target = _tabs.count();
        beginInsertRows({}, target, target);
        _tabs.append(std::move(entry));
        endInsertRows();
    }
    recordLocation(location);
    chooseTab(target);
    return target;
}

bool ViewNavigationSession::activateTab(int index)
{
    const auto* entry = _tabs.at(index);
    if (!entry) return false;
    Location location;
    location.accountId = entry->accountId;
    location.channelId = entry->channelId;
    location.rootId = entry->rootId;
    location.postId = entry->postId;
    location.viewportAnchor = entry->viewportAnchor;
    recordLocation(location);
    chooseTab(index);
    return true;
}

bool ViewNavigationSession::closeTab(int index)
{
    if (!_tabs.at(index)) return false;
    const int previous = _activeTab;
    beginRemoveRows({}, index, index);
    _tabs.remove(index);
    endRemoveRows();
    if (_tabs.isEmpty()) {
        _current = {};
        chooseTab(-1);
    } else if (previous == index) {
        _activeTab = -1;
        activateTab(index < _tabs.count() ? index : _tabs.count() - 1);
    } else if (previous > index) {
        _activeTab = -1;
        chooseTab(previous - 1);
    }
    return true;
}

bool ViewNavigationSession::setPinned(int index, bool pinned)
{
    if (!_tabs.setPinned(index, pinned)) return false;
    emit dataChanged(this->index(index, 0), this->index(index, 0), {PinnedRole});
    return true;
}

bool ViewNavigationSession::updateBookmark(
    int index, const QString& postId,
    std::optional<NavigationTabsModel::ViewportAnchor> anchor)
{
    const auto* entry = _tabs.at(index);
    if (!entry || postId.isEmpty()) return false;
    auto updated = *entry;
    updated.postId = postId;
    updated.viewportAnchor = anchor;
    if (!_tabs.replace(index, std::move(updated))) return false;
    emit dataChanged(this->index(index, 0), this->index(index, 0), {PostIdRole});
    return true;
}

bool ViewNavigationSession::visit(const Location& location)
{
    if (!location.valid()) return false;
    recordLocation(location);
    return true;
}

std::optional<ViewNavigationSession::Location> ViewNavigationSession::goBack()
{
    if (_back.isEmpty()) return std::nullopt;
    pushHistory(_forward, _current);
    _current = _back.takeLast();
    emit historyChanged();
    return _current;
}

std::optional<ViewNavigationSession::Location> ViewNavigationSession::goForward()
{
    if (_forward.isEmpty()) return std::nullopt;
    pushHistory(_back, _current);
    _current = _forward.takeLast();
    emit historyChanged();
    return _current;
}

} // namespace Mattermost
