#pragma once

#include <functional>

#include <QObject>
#include <QString>

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

    // Server-generated labels historically used "<host> — <username>".
    // Do not interpret arbitrary user labels as remote account identities.
    static QString candidateLoginName(const WorkspaceLayout::Account& record);
    static void read(QObject* context, const WorkspaceLayout::Account& record,
                     Callback callback);
};

} // namespace Mattermost
