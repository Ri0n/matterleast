#include "ViewNavigationSession.h"
#include <cmath>
#include <utility>

namespace Mattermost {

ViewNavigationSession::ViewNavigationSession(QString viewId, QObject* parent)
    : QAbstractListModel(parent), _viewId(std::move(viewId))
{
}

ViewNavigationSession::Snapshot ViewNavigationSession::snapshot() const
{
    Snapshot result;
    result.tabs.reserve(_tabs.count());
    for (int i = 0; i < _tabs.count(); ++i)
        result.tabs.append(*_tabs.at(i));
    result.activeTab = _activeTab;
    result.current = _current;
    result.back = _back;
    result.forward = _forward;
    return result;
}

bool ViewNavigationSession::restoreSnapshot(const Snapshot& state)
{
    auto validAnchor = [](const std::optional<NavigationTabsModel::ViewportAnchor>& value) {
        return !value || (value->isValid()
            && std::isfinite(static_cast<double>(value->fraction)));
    };
    const auto& current = state.current;
    const bool emptyCurrent = current.kind == Location::Kind::Chat
        && current.accountId.isEmpty() && current.channelId.isEmpty()
        && current.rootId.isEmpty() && current.postId.isEmpty()
        && !current.viewportAnchor;
    if (state.activeTab < -1 || state.activeTab >= state.tabs.size()
        || (!emptyCurrent && !current.valid())
        || !validAnchor(current.viewportAnchor))
        return false;
    for (int i = 0; i < state.tabs.size(); ++i) {
        const auto& entry = state.tabs.at(i);
        if (!entry.isValid() || entry.accountId.isEmpty()
            || !validAnchor(entry.viewportAnchor)) return false;
        for (int j = 0; j < i; ++j) {
            if (entry.sameDestination(state.tabs.at(j))) return false;
        }
    }
    for (const auto& item : state.back)
        if (!item.valid() || !validAnchor(item.viewportAnchor)) return false;
    for (const auto& item : state.forward)
        if (!item.valid() || !validAnchor(item.viewportAnchor)) return false;

    // Preserve rows representing the same destination, including on reorders.
    // Transferring one view must not reset models consumed by another frontend.
    if (_activeTab >= 0) {
        const int previous = _activeTab;
        _activeTab = -1;
        emit dataChanged(index(previous, 0), index(previous, 0), {ActiveRole});
        emit activeTabChanged(-1);
    }
    for (int i = 0; i < state.tabs.size(); ++i) {
        const auto& wanted = state.tabs.at(i);
        int found = _tabs.findDestination(wanted.channelId, wanted.rootId,
                                          wanted.accountId);
        if (found < 0) {
            beginInsertRows({}, i, i);
            _tabs.insert(i, wanted);
            endInsertRows();
            continue;
        }
        if (found != i) {
            beginMoveRows({}, found, found, {}, i);
            _tabs.move(found, i);
            endMoveRows();
        }
        const auto previous = *_tabs.at(i);
        if (previous.postId != wanted.postId || previous.title != wanted.title
            || previous.pinned != wanted.pinned) {
            _tabs.replace(i, wanted);
            // replace() normally preserves pinning on same-destination
            // navigation; snapshot restoration must preserve the exact flag.
            _tabs.setPinned(i, wanted.pinned);
            emit dataChanged(index(i, 0), index(i, 0),
                             {PostIdRole, TitleRole, PinnedRole});
        }
        if (wanted.viewportAnchor)
            _tabs.setViewportAnchor(i, *wanted.viewportAnchor);
        else
            _tabs.clearViewportAnchor(i);
    }
    for (int i = _tabs.count() - 1; i >= state.tabs.size(); --i) {
        beginRemoveRows({}, i, i);
        _tabs.remove(i);
        endRemoveRows();
    }
    _current = state.current;
    _back = state.back;
    _forward = state.forward;
    constexpr int MaxHistory = 100;
    if (_back.size() > MaxHistory)
        _back.remove(0, _back.size() - MaxHistory);
    if (_forward.size() > MaxHistory)
        _forward.remove(0, _forward.size() - MaxHistory);
    chooseTab(state.activeTab);
    emit historyChanged();
    return true;
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
        _tabs.replace(target, std::move(entry));
        // NavigationTabsModel preserves an existing bookmark on same-channel
        // replace; an explicitly supplied new bookmark must win afterwards.
        if (location.viewportAnchor)
            _tabs.setViewportAnchor(target, *location.viewportAnchor);
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

int ViewNavigationSession::reuseOrOpenChannelTab(
    const Location& location, const QString& title)
{
    if (!location.valid() || location.kind != Location::Kind::Chat
        || !location.rootId.isEmpty()) return -1;
    const int existing = _tabs.findDestination(
        location.channelId, location.rootId, location.accountId);
    if (existing >= 0)
        return openTab(location, title);
    const int reusable = _tabs.findReusableChannelTab(_activeTab);
    if (reusable < 0)
        return openTab(location, title);

    NavigationTabsModel::Entry entry;
    entry.accountId = location.accountId;
    entry.channelId = location.channelId;
    entry.rootId = location.rootId;
    entry.postId = location.postId;
    entry.viewportAnchor = location.viewportAnchor;
    entry.title = title;
    if (!_tabs.replace(reusable, std::move(entry))) return -1;
    emit dataChanged(index(reusable, 0), index(reusable, 0),
                     {AccountIdRole, ChannelIdRole, RootIdRole,
                      PostIdRole, TitleRole, PinnedRole});
    recordLocation(location);
    chooseTab(reusable);
    return reusable;
}

bool ViewNavigationSession::moveTab(int from, int to)
{
    if (from < 0 || from >= _tabs.count() || to < 0
        || to >= _tabs.count() || from == to) return false;
    int selected = _activeTab;
    if (selected == from) selected = to;
    else if (from < selected && selected <= to) --selected;
    else if (to <= selected && selected < from) ++selected;
    const bool selectionMoved = selected != _activeTab;
    beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
    _tabs.move(from, to);
    _activeTab = selected; // observers of rowsMoved see the final roles.
    endMoveRows();
    if (selectionMoved) emit activeTabChanged(selected);
    return true;
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
    if (!_tabs.replace(index, std::move(updated))) return false;
    if (anchor) {
        if (!_tabs.setViewportAnchor(index, *anchor)) return false;
    } else {
        _tabs.clearViewportAnchor(index);
    }
    emit dataChanged(this->index(index, 0), this->index(index, 0), {PostIdRole});
    return true;
}

bool ViewNavigationSession::visit(const Location& location)
{
    if (!location.valid()) return false;
    recordLocation(location);
    const int target = location.kind == Location::Kind::Chat
        ? _tabs.findDestination(location.channelId, location.rootId,
                                location.accountId)
        : -1;
    chooseTab(target);
    return true;
}

std::optional<ViewNavigationSession::Location> ViewNavigationSession::goBack()
{
    if (_back.isEmpty()) return std::nullopt;
    pushHistory(_forward, _current);
    _current = _back.takeLast();
    const int target = _current.kind == Location::Kind::Chat
        ? _tabs.findDestination(_current.channelId, _current.rootId,
                                _current.accountId)
        : -1;
    chooseTab(target);
    emit historyChanged();
    return _current;
}

std::optional<ViewNavigationSession::Location> ViewNavigationSession::goForward()
{
    if (_forward.isEmpty()) return std::nullopt;
    pushHistory(_back, _current);
    _current = _forward.takeLast();
    const int target = _current.kind == Location::Kind::Chat
        ? _tabs.findDestination(_current.channelId, _current.rootId,
                                _current.accountId)
        : -1;
    chooseTab(target);
    emit historyChanged();
    return _current;
}

} // namespace Mattermost
