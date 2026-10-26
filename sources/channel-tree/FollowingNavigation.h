#pragma once

#include "backend/FollowingModel.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

/**
 * Return true when a server-provided resume target is already at or behind the
 * local viewport high-water mark.
 *
 * The server's unread cursor may lag behind local viewport progress until a
 * channel-view acknowledgement reaches it. Navigation must not use such a
 * stale fallback to move a Following/Attention conversation backwards.
 *
 * An uncached target cannot be ordered without fetching its post metadata, so
 * it is not rejected here. The normal navigation resolver may still load it.
 */
inline bool isStaleConversationResumeTarget(const FollowingModel::Entry& entry,
                                            const BackendChannel& channel,
                                            const QString& postId)
{
    if (!entry.hasLocalProgress() || postId.isEmpty()) {
        return false;
    }

    const BackendPost* candidate = channel.postIdToPost.value(postId, nullptr);
    if (!candidate) {
        return false;
    }

    if (candidate->create_at != entry.readThroughCreateAt) {
        return candidate->create_at < entry.readThroughCreateAt;
    }
    return candidate->id <= entry.readThroughPostId;
}

/**
 * Find the oldest definitely unread root already resident in this channel.
 *
 * /posts/unread is not a root-only query: in CRT mode its short reply-only
 * window is not a safe target for parent DM navigation. When a new root is
 * already cached by the realtime path, navigate directly to that root so
 * normal semantic navigation can highlight it, without opening an old thread.
 *
 * Only message timestamps past both the channel's last-viewed boundary and
 * the local viewport high-water mark qualify. Do not infer unread status for
 * the user's own posts.
 */
inline QString firstCachedUnreadRootPostId(const BackendChannel& channel,
                                          std::uint64_t lastViewedAt,
                                          std::uint64_t readThroughCreateAt = 0,
                                          const QString& readThroughPostId = {})
{
    const BackendPost* result = nullptr;
    for (const BackendPost& post : channel.posts) {
        if (post.id.isEmpty() || post.isDeleted || !post.root_id.isEmpty()
            || post.isOwnPost() || post.create_at <= lastViewedAt
            || post.create_at < readThroughCreateAt
            || (post.create_at == readThroughCreateAt
                && post.id <= readThroughPostId)) {
            continue;
        }
        if (!result || post.create_at < result->create_at
            || (post.create_at == result->create_at && post.id < result->id)) {
            result = &post;
        }
    }
    return result ? result->id : QString();
}

} // namespace Mattermost
