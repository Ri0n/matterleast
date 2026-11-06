#pragma once

#include <QtGlobal>

namespace Mattermost {

/** Detect a long interruption in the UI event loop using wall-clock time.
 * CLOCK_MONOTONIC on Linux does not count suspend time; UTC timestamps do.
 */
class ResumeGapDetector
{
public:
    static constexpr qint64 WakeGapMs = 60 * 1000;

    bool sample(qint64 nowMs)
    {
        const bool resumed = lastSampleMs > 0
            && nowMs - lastSampleMs >= WakeGapMs;
        lastSampleMs = nowMs;
        return resumed;
    }

private:
    qint64 lastSampleMs = 0;
};

} // namespace Mattermost
