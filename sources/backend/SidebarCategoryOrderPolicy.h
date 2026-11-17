#pragma once

#include <QStringList>

namespace Mattermost {

// New categories are appended by the server. A "Create category" command
// from an existing category must insert the newly created id immediately
// above that category without disturbing any other order.
inline QStringList sidebarCategoryOrderBefore(const QStringList& existing,
                                              const QString& createdId,
                                              const QString& beforeId)
{
    QStringList order = existing;
    if (createdId.isEmpty()) {
        return order;
    }
    order.removeAll(createdId);
    const int anchor = order.indexOf(beforeId);
    if (!beforeId.isEmpty() && anchor >= 0) {
        order.insert(anchor, createdId);
    } else {
        order.push_back(createdId);
    }
    return order;
}

} // namespace Mattermost
