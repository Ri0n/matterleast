#pragma once

#include <QtGlobal>

namespace Mattermost {

/**
 * The same cadence as the official desktop UserActivityMonitor:
 *   - sample idle time every second;
 *   - activity is idle time <= 60 seconds;
 *   - send active presence at most once a minute;
 *   - forget the last heartbeat when idle, so resuming sends immediately.
 *
 * The server handles away transitions. We deliberately do not manufacture
 * "manual" away or online updates: manually selected statuses take priority.
 */
class PresenceActivityPolicy final
{
public:
    static constexpr qint64 InactiveThresholdMs = 60 * 1000;
    static constexpr qint64 ActiveUpdateIntervalMs = 60 * 1000;

    bool shouldSendActive(qint64 nowMs, qint64 idleMs, bool connected)
    {
        if (!connected) {
            _lastActiveMs = -1;
            return false;
        }
        if (idleMs < 0) {
            // Unknown is NOT evidence of activity. Never mark someone online
            // just because a timer fired or the machine resumed from suspend.
            _lastActiveMs = -1;
            return false;
        }
        if (idleMs > InactiveThresholdMs) {
            _lastActiveMs = -1;
            return false;
        }
        if (_lastActiveMs < 0 || nowMs < _lastActiveMs
            || nowMs - _lastActiveMs >= ActiveUpdateIntervalMs) {
            _lastActiveMs = nowMs;
            return true;
        }
        return false;
    }

    void reset() { _lastActiveMs = -1; }

private:
    qint64 _lastActiveMs = -1;
};

} // namespace Mattermost
