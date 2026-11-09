#pragma once

#include <QQueue>
#include <QSet>
#include <QString>
#include <QtGlobal>

namespace Mattermost {

// Desktop notifications are for live events, not historical WebSocket replay.
// The server's create_at is in milliseconds. History/unread handling must not
// consult this guard: only the desktop popup policy uses it.
class PostNotificationReplayGuard final {
public:
    static constexpr quint64 MaxAgeMs = 5ULL * 60 * 1000;
    static constexpr quint64 AllowedFutureSkewMs = 60ULL * 1000;
    static constexpr int MaxRememberedPosts = 4096;

    bool shouldDeliver(const QString& postId, quint64 createdAtMs, quint64 nowMs)
    {
        if (postId.isEmpty() || createdAtMs == 0 || nowMs == 0) {
            return false;
        }
        if (createdAtMs <= nowMs) {
            if (nowMs - createdAtMs > MaxAgeMs) {
                return false;
            }
        } else if (createdAtMs - nowMs > AllowedFutureSkewMs) {
            return false;
        }

        // WebSocket resume may replay a post already seen while its channel
        // was open. Remember the event even if a later policy suppresses its
        // popup (e.g. because the conversation is currently focused).
        if (_seenPostIds.contains(postId)) {
            return false;
        }
        _seenPostIds.insert(postId);
        _recentPostIds.enqueue(postId);
        if (_recentPostIds.size() > MaxRememberedPosts) {
            _seenPostIds.remove(_recentPostIds.dequeue());
        }
        return true;
    }

private:
    QSet<QString> _seenPostIds;
    QQueue<QString> _recentPostIds;
};

} // namespace Mattermost
