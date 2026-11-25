#include "NavigationSnapshotMigration.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cmath>
#include "navigation/ViewNavigationSession.h"

namespace Mattermost {
namespace {
using Location = ViewNavigationSession::Location;
bool decode(const QJsonObject& object, const QString& accountId, bool anchors,
            Location& location)
{
    location.accountId = accountId;
    location.channelId = object.value("channel").toString();
    location.rootId = object.value("root").toString();
    if (!location.valid()) return false;
    if (!anchors) return true;
    const auto anchor = object.value("anchor");
    if (anchor.isUndefined()) return true;
    if (!anchor.isObject()) return false;
    const auto stored = anchor.toObject();
    const QString post = stored.value("post").toString();
    const auto fraction = stored.value("fraction");
    const auto bottom = stored.value("bottom");
    if (post.isEmpty() || !fraction.isDouble() || !bottom.isBool()
        || !std::isfinite(fraction.toDouble())) return false;
    location.postId = post;
    location.viewportAnchor = NavigationTabsModel::ViewportAnchor {
        post, bottom.toBool(), qreal(fraction.toDouble())};
    return true;
}
}

bool NavigationSnapshotMigration::importLegacy(const QByteArray& json,
                                               const QString& accountId,
                                               ViewNavigationSession& destination)
{
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isObject() || accountId.isEmpty()
        || !destination.allowsAccount(accountId)) return false;
    const auto object = document.object();
    const int version = object.value("version").toInt(-1);
    if ((version != 1 && version != 2) || !object.value("tabs").isArray()
        || !object.value("threads").isArray()) return false;
    auto state = destination.snapshot();
    const int originalCount = state.tabs.size();
    for (const auto& item : object.value("tabs").toArray()) {
        if (!item.isObject()) return false;
        Location location;
        const auto tab = item.toObject();
        if (!decode(tab, accountId, version >= 2, location)) return false;
        NavigationTabsModel::Entry entry;
        entry.accountId = accountId;
        entry.channelId = location.channelId;
        entry.rootId = location.rootId;
        entry.postId = location.postId;
        entry.viewportAnchor = location.viewportAnchor;
        entry.pinned = tab.value("pinned").toBool();
        bool found = false;
        for (const auto& existing : state.tabs)
            if (existing.sameDestination(entry)) { found = true; break; }
        if (!found) state.tabs.append(std::move(entry));
    }
    Location active;
    if (object.value("active_tab").isObject()
        && decode(object.value("active_tab").toObject(), accountId,
                  version >= 2, active)) {
        for (int i = 0; i < state.tabs.size(); ++i)
            if (state.tabs.at(i).accountId == active.accountId
                && state.tabs.at(i).channelId == active.channelId
                && state.tabs.at(i).rootId == active.rootId) {
                if (state.activeTab < 0) state.activeTab = i;
                if (!state.current.valid()) state.current = active;
                break;
            }
    }
    const bool dockVisible = object.value("dock_visible").toBool();
    Location activeDock;
    if (dockVisible && object.value("active_dock").isObject()
        && decode(object.value("active_dock").toObject(), accountId,
                  version >= 2, activeDock)
        && !activeDock.rootId.isEmpty() && !state.dockVisible) {
        state.dock = activeDock;
        state.dockVisible = true;
    }
    for (const auto& item : object.value("threads").toArray()) {
        if (!item.isObject()) return false;
        const auto thread = item.toObject();
        Location location;
        if (!decode(thread, accountId, version >= 2, location)
            || location.rootId.isEmpty()) return false;
        const QString mode = thread.value("mode").toString();
        if (mode == QStringLiteral("window")) {
            const QByteArray encoded = thread.value("geometry").toString().toLatin1();
            if (encoded.size() > 90000) return false;
            bool found = false;
            for (const auto& existing : state.detached)
                if (existing.location.sameDestination(location)) { found = true; break; }
            if (!found) state.detached.append({
                location, QByteArray::fromBase64(encoded)});
        } else if (mode != QStringLiteral("dock")) {
            return false;
        }
    }
    if (state.activeTab < 0 && state.tabs.size() > originalCount)
        state.activeTab = originalCount;
    return destination.restoreSnapshot(state);
}
} // namespace Mattermost
