#pragma once

#include <cstdint>

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

// Search results must not jump when the selected conversation or its unread
// state changes. Only an actual new post changes recency; unknown timestamps
// fall back to alphabetical order, with channel ID as a deterministic tie-break.
inline bool conversationSearchOrderBefore(uint64_t leftLastPostAt,
                                          const QString& leftName,
                                          const QString& leftId,
                                          uint64_t rightLastPostAt,
                                          const QString& rightName,
                                          const QString& rightId)
{
    if (leftLastPostAt != rightLastPostAt) {
        return leftLastPostAt > rightLastPostAt;
    }
    const int nameOrder = QString::compare(leftName, rightName, Qt::CaseInsensitive);
    return nameOrder != 0 ? nameOrder < 0 : leftId < rightId;
}

} // namespace Mattermost
