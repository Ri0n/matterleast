#pragma once

#include <QtGlobal>

namespace Mattermost {

/**
 * Monotonic generation gate for semantic navigation.
 *
 * Every user navigation starts a new generation. Asynchronous callbacks keep
 * the generation they started with and may publish UI state only while it is
 * still current. This makes the latest user-selected destination authoritative
 * even when an older HTTP or repository request completes later.
 */
class NavigationRequestGate
{
public:
    quint64 begin()
    {
        ++generation_;
        if (generation_ == 0) {
            ++generation_;
        }
        return generation_;
    }

    bool isCurrent(quint64 generation) const
    {
        return generation != 0 && generation == generation_;
    }

    quint64 current() const { return generation_; }

private:
    quint64 generation_ = 0;
};

} // namespace Mattermost
