#pragma once

#include <QString>

namespace Mattermost {

// Sidebar filter uses the full known DM/GM directory, not only the visible
// server category subset. The match is deliberately case-insensitive and
// includes user identifiers when a DM's display name is stale.
inline bool matchesConversationSearch(const QString& term,
                                      const QString& channelDisplayName,
                                      const QString& userDisplayName = {},
                                      const QString& username = {},
                                      const QString& email = {})
{
    if (term.trimmed().isEmpty()) return false;
    const QString query = term.trimmed();
    return channelDisplayName.contains(query, Qt::CaseInsensitive)
        || userDisplayName.contains(query, Qt::CaseInsensitive)
        || username.contains(query, Qt::CaseInsensitive)
        || email.contains(query, Qt::CaseInsensitive);
}

} // namespace Mattermost
