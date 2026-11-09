#pragma once

#include <QtGlobal>

namespace Mattermost {

// The server create_at is milliseconds since Unix epoch. A reconnect
// history/backlog event is not a new desktop notification. This policy is
// intentionally separate from post storage, read state and Attention.
inline bool isFreshDesktopPost(quint64 createdAtMs, quint64 nowMs)
{
    constexpr quint64 maxAgeMs = 5ULL * 60 * 1000;
    constexpr quint64 maxFutureSkewMs = 60ULL * 1000;
    if (createdAtMs == 0 || nowMs == 0) {
        return false;
    }
    if (createdAtMs <= nowMs) {
        return nowMs - createdAtMs <= maxAgeMs;
    }
    return createdAtMs - nowMs <= maxFutureSkewMs;
}

/**
 * Skip a post when the server says it was read already.
 * Channel roots and CRT replies have independent server cursors. In legacy
 * (non-root-only) mode, the channel cursor also covers replies. An absent
 * cursor (0) is unknown: do not silently lose a real incoming notification.
 */
inline bool isUnreadDesktopPost(quint64 createdAtMs, bool threadReply,
                               bool rootOnlyChannelUnread,
                               quint64 channelServerReadAtMs,
                               quint64 threadServerReadAtMs)
{
    if (createdAtMs == 0) {
        return false;
    }
    if (!threadReply) {
        return channelServerReadAtMs == 0 || createdAtMs > channelServerReadAtMs;
    }
    if (threadServerReadAtMs != 0 && createdAtMs <= threadServerReadAtMs) {
        return false;
    }
    if (!rootOnlyChannelUnread && channelServerReadAtMs != 0
        && createdAtMs <= channelServerReadAtMs) {
        return false;
    }
    return true;
}

} // namespace Mattermost
