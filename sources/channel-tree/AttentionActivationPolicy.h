#pragma once

#include <cstdint>

#include <QString>

#include "backend/FollowingModel.h"
#include "channel-tree/FollowingPresentation.h"

namespace Mattermost {

// Only inspect already-rendered posts in an activated viewport. An Attention
// entry is a conversation/thread unread domain, not necessarily one post ID.
struct AttentionViewportPost {
    QString channelId;
    QString threadId;          // Empty for a channel/root timeline.
    QString postId;
    QString postRootId;
    std::uint64_t createAt = 0;
    std::uint64_t channelLastViewedAt = 0;
    bool firstUnreadOrLater = false;
};

inline bool shouldFlashAttentionPost(const FollowingModel::Entry& entry,
                                     const AttentionViewportPost& visible)
{
    if (!entry.requiresAttention()
        || !attentionMuteAllowsEntry(entry.muted, entry.isThread() && !entry.synthetic)
        || entry.channelId != visible.channelId) {
        return false;
    }

    if (entry.synthetic) {
        // A synthetic Attention mention points directly to a root message.
        return visible.threadId.isEmpty() && visible.postRootId.isEmpty()
            && entry.threadId == visible.postId;
    }

    if (entry.isThread()) {
        if (visible.threadId != entry.threadId
            || visible.postRootId != entry.threadId) {
            return false;
        }
    } else if (!visible.threadId.isEmpty() || !visible.postRootId.isEmpty()) {
        return false;
    }

    if (!entry.readThroughPostId.isEmpty()) {
        return visible.createAt > entry.readThroughCreateAt
            || (visible.createAt == entry.readThroughCreateAt
                && visible.postId > entry.readThroughPostId);
    }

    if (entry.resumeState == FollowingModel::ResumeState::FirstUnread
        && !entry.firstUnreadPostId.isEmpty()) {
        return visible.firstUnreadOrLater;
    }

    const std::uint64_t lastViewed = entry.isThread()
        ? entry.lastViewedAt : visible.channelLastViewedAt;
    // Unknown boundaries must not cause old, already-read history to flash.
    return lastViewed != 0 && visible.createAt > lastViewed;
}

} // namespace Mattermost
