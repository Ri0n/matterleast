#pragma once

#include <functional>

#include <QObject>
#include <QString>
#include <QVector>

#include "backend/SessionTokenStore.h"
#include "workspaces/WorkspaceLayout.h"

namespace Mattermost {

// Compatibility for catalog records whose saved login name differs from the
// server username used as the QtKeychain key before multi-account migration.
// Added 2026-11-22. Remove after pre-multi-account upgrades are unsupported.
// This class does not authenticate; the caller must verify remote user ID
// before adopting or promoting any recovered token.
class LegacyTokenRecovery final
{
public:
    using Callback = std::function<void(
        SessionTokenStore::ReadStatus status, const QString& token,
        bool fromLegacyDisplayName)>;

    struct Coordinates {
        QString server;
        QString login;
    };
    // Candidate locations are bounded to the same origin and historical
    // verified user label. The caller verifies the exact remote user ID.
    static QVector<Coordinates> lookupCandidates(const WorkspaceLayout::Account& record);
    static QString candidateLoginName(const WorkspaceLayout::Account& record);
    static void read(QObject* context, const WorkspaceLayout::Account& record,
                     Callback callback);
};

} // namespace Mattermost
