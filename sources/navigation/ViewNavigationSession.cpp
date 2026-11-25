#include "ViewNavigationSession.h"
#include <cmath>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <utility>

namespace Mattermost {

namespace {
using Location = ViewNavigationSession::Location;
using Anchor = NavigationTabsModel::ViewportAnchor;
QJsonObject encodeAnchor(const std::optional<Anchor>& anchor)
{
    if (!anchor || !anchor->isValid() || !std::isfinite(double(anchor->fraction)))
        return {};
    return {{"post", anchor->postId}, {"bottom", anchor->bottom},
            {"fraction", double(anchor->fraction)}};
}
bool decodeAnchor(const QJsonValue& value, std::optional<Anchor>& result)
{
    if (value.isUndefined() || value.isNull()) return true;
    if (!value.isObject()) return false;
    const auto object = value.toObject();
    if (!object.value("post").isString() || !object.value("bottom").isBool()
        || !object.value("fraction").isDouble()) return false;
    Anchor anchor {object.value("post").toString(),
                   object.value("bottom").toBool(),
                   qreal(object.value("fraction").toDouble())};
    if (!anchor.isValid() || !std::isfinite(double(anchor.fraction))) return false;
    result = anchor;
    return true;
}
QJsonObject encodeLocation(const Location& location)
{
    QJsonObject object {{"kind", int(location.kind)}, {"account", location.accountId},
                        {"channel", location.channelId}, {"root", location.rootId},
                        {"post", location.postId}};
    if (location.viewportAnchor)
        object.insert("anchor", encodeAnchor(location.viewportAnchor));
    return object;
}
bool decodeLocation(const QJsonValue& value, Location& result, bool allowEmpty)
{
    if (allowEmpty && value.isNull()) return true;
    if (!value.isObject()) return false;
    const auto object = value.toObject();
    const auto kind = object.value("kind");
    if (!kind.isDouble() || kind.toInt(-1) < 0
        || kind.toInt(-1) > int(Location::Kind::Search)
        || !object.value("account").isString()
        || !object.value("channel").isString()
        || !object.value("root").isString()
        || !object.value("post").isString()) return false;
    result.kind = static_cast<Location::Kind>(kind.toInt());
    result.accountId = object.value("account").toString();
    result.channelId = object.value("channel").toString();
    result.rootId = object.value("root").toString();
    result.postId = object.value("post").toString();
    return decodeAnchor(object.value("anchor"), result.viewportAnchor)
        && result.valid();
}
QJsonArray encodeHistory(const QVector<Location>& locations)
{
    QJsonArray array;
    for (const auto& location : locations) array.append(encodeLocation(location));
    return array;
}
bool decodeHistory(const QJsonValue& value, QVector<Location>& locations)
{
    if (!value.isArray() || value.toArray().size() > 100) return false;
    for (const auto& item : value.toArray()) {
        Location location;
        if (!decodeLocation(item, location, false)) return false;
        locations.append(std::move(location));
    }
    return true;
}
}

QByteArray ViewNavigationSession::serializeSnapshot() const
{
    const auto state = snapshot();
    QJsonArray tabs;
    for (const auto& entry : state.tabs) {
        QJsonObject tab {{"account", entry.accountId}, {"channel", entry.channelId},
                         {"root", entry.rootId}, {"post", entry.postId},
                         {"title", entry.title}, {"pinned", entry.pinned}};
        if (entry.viewportAnchor)
            tab.insert("anchor", encodeAnchor(entry.viewportAnchor));
        tabs.append(tab);
    }
    QJsonArray detached;
    for (const auto& window : state.detached) {
        detached.append(QJsonObject {
            {"location", encodeLocation(window.location)},
            {"geometry", QString::fromLatin1(window.geometry.toBase64())}
        });
    }
    const QJsonObject document {
        {"version", 1}, {"view", _viewId}, {"tabs", tabs},
        {"active", state.activeTab},
        {"current", state.current.valid()
            ? QJsonValue(encodeLocation(state.current)) : QJsonValue(QJsonValue::Null)},
        {"back", encodeHistory(state.back)}, {"forward", encodeHistory(state.forward)},
        {"dock", state.dock.valid()
            ? QJsonValue(encodeLocation(state.dock)) : QJsonValue(QJsonValue::Null)},
        {"dock_visible", state.dockVisible}, {"detached", detached}
    };
    return QJsonDocument(document).toJson(QJsonDocument::Compact);
}

bool ViewNavigationSession::restoreSerializedSnapshot(const QByteArray& json)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const auto object = document.object();
    if (object.value("version").toInt(-1) != 1
        || object.value("view").toString() != _viewId
        || !object.value("tabs").isArray()
        || !object.value("active").isDouble()
        || !object.value("dock_visible").isBool()) return false;
    const auto tabArray = object.value("tabs").toArray();
    if (tabArray.size() > 1000) return false;
    Snapshot state;
    state.activeTab = object.value("active").toInt(-2);
    state.dockVisible = object.value("dock_visible").toBool();
    for (const auto& value : tabArray) {
        if (!value.isObject()) return false;
        const auto tab = value.toObject();
        if (!tab.value("account").isString()
            || !tab.value("channel").isString()
            || !tab.value("root").isString()
            || !tab.value("post").isString()
            || !tab.value("title").isString()
            || !tab.value("pinned").isBool()) return false;
        NavigationTabsModel::Entry entry;
        entry.accountId = tab.value("account").toString();
        entry.channelId = tab.value("channel").toString();
        entry.rootId = tab.value("root").toString();
        entry.postId = tab.value("post").toString();
        entry.title = tab.value("title").toString();
        entry.pinned = tab.value("pinned").toBool();
        if (!decodeAnchor(tab.value("anchor"), entry.viewportAnchor)) return false;
        state.tabs.append(std::move(entry));
    }
    const auto windows = object.value("detached");
    if (!windows.isUndefined()) {
        if (!windows.isArray() || windows.toArray().size() > 100) return false;
        for (const auto& item : windows.toArray()) {
            if (!item.isObject()) return false;
            const auto window = item.toObject();
            if (!window.value("geometry").isString()) return false;
            DetachedThread detached;
            if (!decodeLocation(window.value("location"), detached.location, false))
                return false;
            const QByteArray encoded = window.value("geometry").toString().toLatin1();
            if (encoded.size() > 90000) return false;
            detached.geometry = QByteArray::fromBase64(encoded);
            state.detached.append(std::move(detached));
        }
    }
    if (!decodeLocation(object.value("current"), state.current, true)
        || !decodeLocation(object.value("dock"), state.dock, true)
        || !decodeHistory(object.value("back"), state.back)
        || !decodeHistory(object.value("forward"), state.forward)) return false;
    // The model validates all identities and anchors before changing any rows.
    return restoreSnapshot(state);
}

ViewNavigationSession::ViewNavigationSession(QString viewId, QObject* parent)
    : QAbstractListModel(parent), _viewId(std::move(viewId))
{
}

bool ViewNavigationSession::allowsAccount(const QString& accountId) const
{
    return !_membershipRestricted || _allowedAccounts.contains(accountId);
}

void ViewNavigationSession::setAllowedAccounts(const QStringList& accountIds)
{
    const QSet<QString> allowed(accountIds.cbegin(), accountIds.cend());
    if (_membershipRestricted && _allowedAccounts == allowed) return;

    auto state = snapshot();
    QVector<NavigationTabsModel::Entry> kept;
    kept.reserve(state.tabs.size());
    int selected = -1;
    for (int i = 0; i < state.tabs.size(); ++i) {
        if (!allowed.contains(state.tabs[i].accountId)) continue;
        if (i == state.activeTab) selected = kept.size();
        kept.append(state.tabs.at(i));
    }
    state.tabs = std::move(kept);
    state.activeTab = selected;
    auto belongs = [&allowed](const Location& location) {
        return allowed.contains(location.accountId);
    };
    if (state.current.valid() && !belongs(state.current)) state.current = {};
    auto retainHistory = [&belongs](QVector<Location>& entries) {
        for (int i = entries.size() - 1; i >= 0; --i)
            if (!belongs(entries[i])) entries.removeAt(i);
    };
    retainHistory(state.back);
    retainHistory(state.forward);
    if (state.dock.valid() && !belongs(state.dock)) {
        state.dock = {};
        state.dockVisible = false;
    }
    for (int i = state.detached.size() - 1; i >= 0; --i)
        if (!belongs(state.detached[i].location)) state.detached.removeAt(i);
    _allowedAccounts = allowed;
    _membershipRestricted = true;
    // Snapshot is guaranteed valid after filtering account membership; never
    // reconstruct the model or recreate dependent QWidget presentations.
    restoreSnapshot(state);
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
    result.dock = _dock;
    result.dockVisible = _dockVisible;
    result.detached = _detached;
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
        || (!emptyCurrent && !allowsAccount(current.accountId))
        || !validAnchor(current.viewportAnchor))
        return false;
    for (int i = 0; i < state.tabs.size(); ++i) {
        const auto& entry = state.tabs.at(i);
        if (!entry.isValid() || entry.accountId.isEmpty()
            || !allowsAccount(entry.accountId)
            || !validAnchor(entry.viewportAnchor)) return false;
        for (int j = 0; j < i; ++j) {
            if (entry.sameDestination(state.tabs.at(j))) return false;
        }
    }
    for (const auto& item : state.back)
        if (!item.valid() || !allowsAccount(item.accountId)
            || !validAnchor(item.viewportAnchor)) return false;
    for (const auto& item : state.forward)
        if (!item.valid() || !allowsAccount(item.accountId)
            || !validAnchor(item.viewportAnchor)) return false;
    if (state.detached.size() > 100) return false;
    for (int i = 0; i < state.detached.size(); ++i) {
        const auto& detached = state.detached.at(i);
        if (!detached.location.valid() || !allowsAccount(detached.location.accountId)
            || detached.location.kind != Location::Kind::Chat
            || detached.location.rootId.isEmpty()
            || !validAnchor(detached.location.viewportAnchor)
            || detached.geometry.size() > 65536) return false;
        for (int j = 0; j < i; ++j)
            if (detached.location.sameDestination(state.detached[j].location))
                return false;
    }
    if (state.dockVisible
        && (!state.dock.valid()
            || !allowsAccount(state.dock.accountId)
            || state.dock.kind != Location::Kind::Chat
            || state.dock.rootId.isEmpty()
            || !validAnchor(state.dock.viewportAnchor))) return false;

    // Preserve the active row on routine bookmark/history synchronization.
    // Toggling it off and back on for every snapshot generated redundant
    // activeTabChanged signals to Widgets/QML even when no tab had changed.
    bool sameLayout = _tabs.count() == state.tabs.size();
    if (sameLayout) {
        for (int i = 0; i < _tabs.count(); ++i) {
            if (!_tabs.at(i)->sameDestination(state.tabs.at(i))) {
                sameLayout = false;
                break;
            }
        }
    }
    // Preserve rows representing the same destination, including on reorders.
    // Transferring one view must not reset models consumed by another frontend.
    if (!sameLayout && _activeTab >= 0) {
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
    _dock = state.dock;
    _dockVisible = state.dockVisible;
    _detached = state.detached;
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
    if (!location.valid() || !allowsAccount(location.accountId)
        || location.kind != Location::Kind::Chat)
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
    if (!location.valid() || !allowsAccount(location.accountId)
        || location.kind != Location::Kind::Chat
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

bool ViewNavigationSession::recordVisit(const Location& location)
{
    if (!location.valid() || !allowsAccount(location.accountId)) return false;
    recordLocation(location);
    return true;
}

bool ViewNavigationSession::updateCurrentBookmark(const Location& location)
{
    if (!location.valid() || !allowsAccount(location.accountId)
        || (_current.valid()
        && !_current.sameDestination(location))) return false;
    _current = location;
    return true;
}

bool ViewNavigationSession::visit(const Location& location)
{
    if (!location.valid() || !allowsAccount(location.accountId)) return false;
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
