#pragma once

#include <QString>

namespace Mattermost {

// Stable non-secret identity for one server-assigned user on one Mattermost
// origin/base path. Shared by runtime registration and persisted workspace
// validation to avoid divergent URL normalization rules.
QString accountIdentityKey(const QString& serverBaseUrl,
                           const QString& serverUserId);

} // namespace Mattermost
