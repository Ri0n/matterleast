#pragma once

#include <QByteArray>
#include <QString>

#include "WorkspaceLayout.h"

namespace Mattermost {

// Durable, credential-free account catalog. It does not own Backends, caches,
// network retries or widgets. The application chooses how to persist its JSON.
class AccountRegistry final
{
public:
    bool load(const QByteArray& json, QString* error = nullptr);
    QByteArray toJson() const { return _layout.toJson(); }
    bool empty() const { return _layout.accounts.isEmpty(); }

    const WorkspaceLayout& layout() const { return _layout; }
    const WorkspaceLayout::Account* account(const QString& id) const;

    // Returns a stable local account ID; empty on invalid/duplicate layout.
    // Reauthentication updates the keyring lookup name but keeps the local ID,
    // view membership and display name.
    QString registerAuthenticated(const QString& serverBaseUrl,
                                  const QString& serverUserId,
                                  const QString& loginName,
                                  const QString& displayName);
    bool selectAccount(const QString& id);

private:
    WorkspaceLayout _layout;
};

} // namespace Mattermost
