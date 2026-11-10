/**
 * @file DeletedUnreadPostPolicy.h
 * @brief Decide whether a fresh post-since response proves an unread channel is empty.
 */
#pragma once

#include <cstdint>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "PostVisibilityPolicy.h"

namespace Mattermost {

enum class DeletedUnreadPostCheck {
    Inconclusive,
    HasUnreadPosts,
    NoUnreadPosts,
};

// A read post, own post, or CRT-only reply cannot be the cause of a parent
// channel's remaining unread state. Unknown event metadata is conservative.
inline bool deletionMayAffectChannelUnread(uint64_t createdAt,
                                           uint64_t lastViewedAt,
                                           const QString& authorId,
                                           const QString& ownUserId,
                                           bool isReply,
                                           bool rootOnly)
{
    return (createdAt == 0 || createdAt > lastViewedAt)
        && (authorId.isEmpty() || authorId != ownUserId)
        && !(isReply && rootOnly);
}

// Query /channels/{id}/posts?since={last_viewed_at}, NOT /posts/unread:
// the latter falls back to an ordinary history page when there are no unread
// posts. A missing or partial response must never trigger a read receipt.
inline DeletedUnreadPostCheck checkLiveUnreadPostsAfterDeletion(
    const QJsonObject& response, uint64_t lastViewedAt, const QString& ownUserId)
{
    if (!response.value(QStringLiteral("order")).isArray()
        || !response.value(QStringLiteral("posts")).isObject()
        || ownUserId.isEmpty()) {
        return DeletedUnreadPostCheck::Inconclusive;
    }

    const QJsonArray order = response.value(QStringLiteral("order")).toArray();
    const QJsonObject posts = response.value(QStringLiteral("posts")).toObject();

    // An ordered ID without a body means we cannot certify the window.
    for (const QJsonValue& value : order) {
        const QString id = value.toString();
        if (id.isEmpty() || !posts.value(id).isObject()) {
            return DeletedUnreadPostCheck::Inconclusive;
        }
    }

    // Check every returned body, not just order: the server can also include
    // thread metadata not represented by the flat order.
    for (auto it = posts.constBegin(); it != posts.constEnd(); ++it) {
        if (!it->isObject()) {
            return DeletedUnreadPostCheck::Inconclusive;
        }
        const QJsonObject post = it->toObject();
        const uint64_t createdAt =
            post.value(QStringLiteral("create_at")).toVariant().toULongLong();
        if (createdAt == 0) {
            return DeletedUnreadPostCheck::Inconclusive;
        }
        if (createdAt <= lastViewedAt
            || post.value(QStringLiteral("delete_at")).toVariant().toULongLong() != 0
            || post.value(QStringLiteral("user_id")).toString() == ownUserId
            || isRoutineMembershipPostType(post.value(QStringLiteral("type")).toString())) {
            continue;
        }
        return DeletedUnreadPostCheck::HasUnreadPosts;
    }

    return DeletedUnreadPostCheck::NoUnreadPosts;
}

} // namespace Mattermost
