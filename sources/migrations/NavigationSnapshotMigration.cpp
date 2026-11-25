#include "NavigationSnapshotMigration.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "navigation/ViewNavigationSession.h"

namespace Mattermost {

bool NavigationSnapshotMigration::importLegacy(const QByteArray& json,
                                               const QString& accountId,
                                               ViewNavigationSession& destination)
{
    const auto document = QJsonDocument::fromJson(json);
    if (!document.isObject() || accountId.isEmpty()
        || !destination.allowsAccount(accountId)) return false;
    const auto object = document.object();
    const int version = object.value("version").toInt(-1);
    if ((version != 1 && version != 2) || !object.value("tabs").isArray())
        return false;
    auto state = destination.snapshot();
    for (const auto& item : object.value("tabs").toArray()) {
        if (!item.isObject()) return false;
        const auto tab = item.toObject();
        if (!tab.value("pinned").toBool()) continue;
        NavigationTabsModel::Entry entry;
        entry.accountId = accountId;
        entry.channelId = tab.value("channel").toString();
        entry.rootId = tab.value("root").toString();
        entry.pinned = true;
        if (!entry.isValid()) return false;
        bool found = false;
        for (auto& existing : state.tabs) {
            if (existing.sameDestination(entry)) {
                existing.pinned = true;
                found = true;
                break;
            }
        }
        if (!found) state.tabs.append(std::move(entry));
    }
    return destination.restoreSnapshot(state);
}
} // namespace Mattermost
