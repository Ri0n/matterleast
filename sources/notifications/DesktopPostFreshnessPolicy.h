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

} // namespace Mattermost
