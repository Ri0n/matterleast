/**
 * @file ChannelActivityTracker.cpp
 * @brief Tracks recent and unread channel activity independently of the UI.
 */

#include "ChannelActivityTracker.h"

#include <algorithm>

namespace Mattermost {

void ChannelActivityTracker::clear()
{
    entries.clear();
}

void ChannelActivityTracker::setMembership(const QString& channelId, uint64_t lastViewedAt,
                                           uint64_t readMessageCount, uint64_t readRootMessageCount,
                                           bool hasReadRootMessageCount, uint64_t mentionCount,
                                           uint64_t rootMentionCount, bool hasRootMentionCount,
                                           bool muted)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.lastViewedAt = std::max(entry.lastViewedAt, lastViewedAt);
    entry.readMessageCount = std::max(entry.readMessageCount, readMessageCount);
    if (hasReadRootMessageCount) {
        entry.readRootMessageCount = std::max(entry.readRootMessageCount, readRootMessageCount);
        entry.hasReadRootMessageCount = true;
    }

    entry.mentionCount = mentionCount;
    entry.rootMentionCount = rootMentionCount;
    entry.hasRootMentionCount = hasRootMentionCount;
    entry.muted = muted;
    entry.tracked = true;
}

void ChannelActivityTracker::synchronizeChannel(const QString& channelId, uint64_t lastPostAt,
                                                uint64_t totalMessageCount,
                                                uint64_t totalRootMessageCount,
                                                bool hasTotalRootMessageCount,
                                                bool useCollapsedThreads)
{
    auto it = entries.find(channelId);
    if (it == entries.end() || !it->tracked) {
        return;
    }

    Entry& entry = it.value();
    entry.lastActivityAt = std::max(entry.lastActivityAt, lastPostAt);

    entry.rootUnreadMode = useCollapsedThreads
        && entry.hasReadRootMessageCount
        && hasTotalRootMessageCount;
    const uint64_t readCount = entry.rootUnreadMode
        ? entry.readRootMessageCount : entry.readMessageCount;
    const uint64_t totalCount = entry.rootUnreadMode
        ? totalRootMessageCount : totalMessageCount;

    entry.serverUnreadActivity = totalCount > readCount;
    entry.serverMentioned = entry.rootUnreadMode && entry.hasRootMentionCount
        ? entry.rootMentionCount > 0
        : entry.mentionCount > 0;
}

void ChannelActivityTracker::recordPost(const QString& channelId, uint64_t createdAt, bool ownPost,
                                        bool threadReply, bool mentioned)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.tracked = true;
    entry.lastActivityAt = std::max(entry.lastActivityAt, createdAt);

    // Preserve both runtime domains. Whether reply activity belongs to the
    // parent is decided by the same root-counter mode as server unread
    // state, so a late membership/channel snapshot can safely change the
    // selected domain without losing or inventing WS activity.
    if (mentioned) {
        if (threadReply) {
            entry.runtimeReplyMentioned = true;
        } else {
            entry.runtimeMentioned = true;
        }
    }

    if (ownPost) {
        return;
    }

    if (threadReply) {
        entry.runtimeReplyUnreadActivity = true;
    } else {
        entry.runtimeUnreadActivity = true;
    }
}

void ChannelActivityTracker::recordViewed(const QString& channelId, uint64_t viewedAt,
                                          uint64_t totalMessageCount,
                                          uint64_t totalRootMessageCount,
                                          bool hasTotalRootMessageCount)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.tracked = true;
    entry.lastViewedAt = std::max(entry.lastViewedAt, viewedAt);

    if (entry.rootUnreadMode) {
        // A root-only channel acknowledgement consumes only the parent/root
        // domain. Keep all-message counters and reply runtime activity intact:
        // they still belong to followed threads and must become visible again
        // if root counters later become unavailable and fallback semantics apply.
        if (hasTotalRootMessageCount) {
            entry.readRootMessageCount = std::max(
                entry.readRootMessageCount, totalRootMessageCount);
            entry.hasReadRootMessageCount = true;
        }
        entry.rootMentionCount = 0;
        entry.serverUnreadActivity = false;
        entry.runtimeUnreadActivity = false;
        entry.serverMentioned = false;
        entry.runtimeMentioned = false;
        return;
    }

    entry.readMessageCount = std::max(entry.readMessageCount, totalMessageCount);
    if (hasTotalRootMessageCount) {
        entry.readRootMessageCount = std::max(entry.readRootMessageCount, totalRootMessageCount);
        entry.hasReadRootMessageCount = true;
    }

    entry.mentionCount = 0;
    entry.rootMentionCount = 0;
    entry.serverUnreadActivity = false;
    entry.runtimeUnreadActivity = false;
    entry.runtimeReplyUnreadActivity = false;
    entry.serverMentioned = false;
    entry.runtimeMentioned = false;
    entry.runtimeReplyMentioned = false;
}

void ChannelActivityTracker::markUnread(const QString& channelId,
                                        uint64_t lastViewedAt,
                                        uint64_t readMessageCount,
                                        uint64_t readRootMessageCount,
                                        bool hasReadRootMessageCount,
                                        uint64_t mentionCount,
                                        uint64_t rootMentionCount,
                                        bool hasRootMentionCount)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.tracked = true;

    // Unlike ordinary membership refreshes, set_unread is explicitly allowed
    // to move the user's read watermark backwards.
    entry.lastViewedAt = lastViewedAt;
    entry.readMessageCount = readMessageCount;
    if (hasReadRootMessageCount) {
        entry.readRootMessageCount = readRootMessageCount;
        entry.hasReadRootMessageCount = true;
    }

    entry.mentionCount = mentionCount;
    entry.rootMentionCount = rootMentionCount;
    entry.hasRootMentionCount = hasRootMentionCount;
    entry.serverUnreadActivity = true;
    entry.runtimeUnreadActivity = false;
    entry.runtimeReplyUnreadActivity = false;
    entry.serverMentioned = entry.rootUnreadMode && hasRootMentionCount
        ? rootMentionCount > 0
        : mentionCount > 0;
    entry.runtimeMentioned = false;
    entry.runtimeReplyMentioned = false;
}

void ChannelActivityTracker::setRecencyTimes(const QString& channelId, uint64_t approximateViewAt,
                                             uint64_t openTimeAt)
{
    if (channelId.isEmpty()) {
        return;
    }

    Entry& entry = entries[channelId];
    entry.approximateViewAt = std::max(entry.approximateViewAt, approximateViewAt);
    entry.openTimeAt = std::max(entry.openTimeAt, openTimeAt);
}

void ChannelActivityTracker::setOpenTime(const QString& channelId, uint64_t openTimeAt)
{
    if (channelId.isEmpty()) {
        return;
    }
    entries[channelId].openTimeAt = std::max(entries[channelId].openTimeAt, openTimeAt);
}

void ChannelActivityTracker::setMentioned(const QString& channelId, bool mentioned)
{
    auto it = entries.find(channelId);
    if (it == entries.end()) {
        return;
    }

    if (mentioned) {
        it->runtimeMentioned = true;
    } else {
        it->mentionCount = 0;
        it->rootMentionCount = 0;
        it->serverMentioned = false;
        it->runtimeMentioned = false;
        it->runtimeReplyMentioned = false;
    }
}

void ChannelActivityTracker::setMuted(const QString& channelId, bool muted)
{
    auto it = entries.find(channelId);
    if (it == entries.end()) {
        return;
    }
    it->muted = muted;
}

bool ChannelActivityTracker::isTracked(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    return it != entries.cend() && it->tracked;
}

bool ChannelActivityTracker::isUnread(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    if (it == entries.cend() || !it->tracked) {
        return false;
    }

    const Entry& entry = it.value();
    const bool mentioned = entry.serverMentioned || entry.runtimeMentioned
        || (!entry.rootUnreadMode && entry.runtimeReplyMentioned);
    const bool unreadActivity = entry.serverUnreadActivity || entry.runtimeUnreadActivity
        || (!entry.rootUnreadMode && entry.runtimeReplyUnreadActivity);
    return mentioned || (!entry.muted && unreadActivity);
}

bool ChannelActivityTracker::hasMention(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    if (it == entries.cend()) {
        return false;
    }
    return it->serverMentioned || it->runtimeMentioned
        || (!it->rootUnreadMode && it->runtimeReplyMentioned);
}

bool ChannelActivityTracker::usesRootUnreadCounts(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    return it != entries.cend() && it->tracked && it->rootUnreadMode;
}

uint64_t ChannelActivityTracker::activityTime(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    if (it == entries.cend() || !it->tracked) {
        return 0;
    }

    return std::max(it->lastViewedAt, it->lastActivityAt);
}

uint64_t ChannelActivityTracker::lastViewedTime(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    if (it == entries.cend() || !it->tracked) {
        return 0;
    }

    return it->lastViewedAt;
}

uint64_t ChannelActivityTracker::recentTime(const QString& channelId) const
{
    const auto it = entries.constFind(channelId);
    if (it == entries.cend() || !it->tracked) {
        return 0;
    }

    return std::max({it->lastViewedAt, it->approximateViewAt, it->openTimeAt});
}

} // namespace Mattermost
