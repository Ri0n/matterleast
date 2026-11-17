#pragma once

#include <QSet>
#include <QStringList>

namespace Mattermost {

// A sorted category's channel_ids are server membership/order data, not the
// currently displayed order. Before switching to manual, materialize the
// exact visible order so a drag insertion has meaningful adjacent items.
// Preserve hidden, capped and not-yet-resolved channels at the tail in their
// original relative order; never lose membership during this transition.
inline QStringList sidebarManualOrder(const QStringList& stored,
                                      const QStringList& visible)
{
    QStringList result;
    result.reserve(stored.size());
    QSet<QString> members;
    for (const QString& id : stored) {
        members.insert(id);
    }
    QSet<QString> seen;
    for (const QString& id : visible) {
        if (members.contains(id) && !seen.contains(id)) {
            result.push_back(id);
            seen.insert(id);
        }
    }
    for (const QString& id : stored) {
        if (!seen.contains(id)) {
            result.push_back(id);
            seen.insert(id);
        }
    }
    return result;
}

} // namespace Mattermost
