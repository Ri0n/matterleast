#pragma once

#include <QString>
#include <QStringList>

namespace Mattermost {

// Built-in Favorites is a valid server-side destination, unlike the
// automatic Channels and Direct Messages categories. A category must never
// list itself as a destination in its own "Move to category" submenu.
inline bool sidebarCategoryIsMoveTarget(const QString& type,
                                       const QString& categoryId,
                                       const QString& sourceCategoryId)
{
    return !categoryId.isEmpty() && categoryId != sourceCategoryId
        && (type == QLatin1String("custom")
            || type == QLatin1String("favorites"));
}

inline bool reorderSidebarChannel(QStringList& channelIds,
                                  const QString& channelId,
                                  const QString& targetChannelId,
                                  bool afterTarget)
{
    const QStringList before = channelIds;
    const int oldIndex = channelIds.indexOf(channelId);
    if (oldIndex < 0) {
        return false;
    }

    channelIds.removeAt(oldIndex);
    int insertIndex = channelIds.size();
    if (!targetChannelId.isEmpty()) {
        const int targetIndex = channelIds.indexOf(targetChannelId);
        if (targetIndex >= 0) {
            insertIndex = targetIndex + (afterTarget ? 1 : 0);
        }
    }
    channelIds.insert(insertIndex, channelId);
    return channelIds != before;
}

inline bool reorderSidebarCategory(QStringList& categoryIds,
                                   const QString& categoryId,
                                   const QString& targetCategoryId,
                                   bool afterTarget)
{
    const QStringList before = categoryIds;
    const int oldIndex = categoryIds.indexOf(categoryId);
    if (oldIndex < 0 || targetCategoryId.isEmpty() || categoryId == targetCategoryId) {
        return false;
    }

    categoryIds.removeAt(oldIndex);
    const int targetIndex = categoryIds.indexOf(targetCategoryId);
    if (targetIndex < 0) {
        categoryIds = before;
        return false;
    }

    categoryIds.insert(targetIndex + (afterTarget ? 1 : 0), categoryId);
    return categoryIds != before;
}

inline bool moveSidebarChannel(QStringList& sourceChannelIds,
                               QStringList& targetChannelIds,
                               const QString& channelId,
                               const QString& targetChannelId = {},
                               bool afterTarget = false)
{
    if (channelId.isEmpty()) {
        return false;
    }

    const QStringList sourceBefore = sourceChannelIds;
    const QStringList targetBefore = targetChannelIds;
    sourceChannelIds.removeAll(channelId);
    targetChannelIds.removeAll(channelId);

    int insertIndex = targetChannelIds.size();
    if (!targetChannelId.isEmpty()) {
        const int targetIndex = targetChannelIds.indexOf(targetChannelId);
        if (targetIndex >= 0) {
            insertIndex = targetIndex + (afterTarget ? 1 : 0);
        }
    }
    targetChannelIds.insert(insertIndex, channelId);
    return sourceChannelIds != sourceBefore || targetChannelIds != targetBefore;
}

} // namespace Mattermost
