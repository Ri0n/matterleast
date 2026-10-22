/**
 * @file ChannelUnreadPostSelection.h
 * @brief Select a safe unread navigation target from a Mattermost PostList.
 */
#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace Mattermost {

/**
 * /posts/unread is an all-post timeline response, even for channels whose
 * sidebar unread status is defined by CRT root counters. Its oldest returned
 * post can therefore be a reply from a previously read thread. Opening that
 * reply via openPost() incorrectly navigates into the thread, instead of the
 * unread parent conversation.
 *
 * For root-only navigation accept only a post whose root_id is explicitly
 * empty in the same server response. If the short unread window contains no
 * verified root, return no target: the caller can open the channel's latest
 * root timeline instead of jumping to an unrelated thread reply.
 */
inline QString selectChannelUnreadPostId(const QJsonObject& response,
                                         bool rootsOnly)
{
    const QJsonArray order = response.value(QStringLiteral("order")).toArray();
    const QJsonObject posts = response.value(QStringLiteral("posts")).toObject();
    for (int i = order.size() - 1; i >= 0; --i) {
        const QString postId = order.at(i).toString();
        if (postId.isEmpty()) {
            continue;
        }
        if (!rootsOnly) {
            return postId;
        }
        const QJsonValue value = posts.value(postId);
        if (!value.isObject()) {
            continue; // Missing metadata is not proof of a root post.
        }
        const QJsonObject post = value.toObject();
        if (post.contains(QStringLiteral("root_id"))
            && post.value(QStringLiteral("root_id")).isString()
            && post.value(QStringLiteral("root_id")).toString().isEmpty()) {
            return postId;
        }
    }
    return {};
}

/** A cached FirstUnread cursor must obey the same parent/root boundary. */
inline bool isConversationTargetCompatible(bool rootsOnly,
                                           bool postMetadataKnown,
                                           const QString& rootId)
{
    return !rootsOnly || (postMetadataKnown && rootId.isEmpty());
}

} // namespace Mattermost
