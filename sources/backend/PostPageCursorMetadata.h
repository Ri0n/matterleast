#pragma once

#include <cstdint>

#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <QVariant>

namespace Mattermost {

/**
 * Extract source-level compound-cursor metadata directly from a REST/cache page.
 *
 * BackendPost residency is intentionally irrelevant here. A thread source may
 * need to keep walking from an off-screen page after its heavyweight bodies have
 * already been evicted, while post id + create_at are sufficient for Mattermost's
 * thread cursor.
 */
inline QHash<QString, std::uint64_t> postPageCursorCreateAtById(
    const QJsonObject& postsObject,
    const QStringList& postIds)
{
    QHash<QString, std::uint64_t> result;
    result.reserve(postIds.size());
    for (const QString& id : postIds) {
        const QJsonValue value = postsObject.value(id);
        if (!value.isObject()) {
            continue;
        }
        const std::uint64_t createAt = value.toObject()
            .value(QStringLiteral("create_at")).toVariant().toULongLong();
        if (createAt != 0) {
            result.insert(id, createAt);
        }
    }
    return result;
}

} // namespace Mattermost
