#pragma once
#include <QByteArray>
#include <QString>
namespace Mattermost {
class ViewNavigationSession;
// 2026-11-25: one-time conversion of the pre-View v1/v2 account snapshots.
// The caller writes the new snapshot before retiring the legacy settings key.
class NavigationSnapshotMigration final
{
public:
    static bool importLegacy(const QByteArray& json, const QString& accountId,
                             ViewNavigationSession& destination);
};
}
