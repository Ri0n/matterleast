#pragma once

#include <algorithm>
#include <utility>

#include <QVector>

namespace Mattermost {

/**
 * Coalesces logical demand around one in-flight exact boundary request.
 *
 * The gate deliberately does not queue transport work. A request may join
 * an in-flight operation ONLY when the physical response is guaranteed to
 * cover the request's entire interval. Merely overlapping/touching intervals
 * are not enough: if the view filters away some of the first response's rows,
 * LongListWidget can stop issuing requests, leaving an adjacent uncached
 * range permanently inaccessible. Uncovered demand must run its own transport.
 * This also lets distant/random requests bypass stale scrolling chains.
 */
class PostSourceRequestGate
{
public:
    struct Range {
        int first = -1;
        int last = -1;

        bool isValid() const { return first >= 0 && last >= first; }
    };

    bool isActive() const { return expected.isValid(); }

    bool canAttach(int first, int last) const
    {
        if (!isActive() || !coverageCurrent || last < first) {
            return false;
        }
        return first >= expected.first && last <= expected.last;
    }

    void begin(int expectedFirst,
               int expectedLast,
               int requestFirst,
               int requestLast)
    {
        coverageCurrent = true;
        expected.first = std::min(expectedFirst, expectedLast);
        expected.last = std::max(expectedFirst, expectedLast);
        waiters.clear();
        attach(requestFirst, requestLast);
    }

    // Structural remaps invalidate numeric coverage, but original request
    // coordinates must survive until the owning transport finishes.
    void invalidateCoverage() { coverageCurrent = false; }

    void attach(int first, int last)
    {
        if (last >= first) {
            waiters.push_back(Range { first, last });
        }
    }

    const Range& expectedRange() const { return expected; }

    QVector<Range> finish()
    {
        QVector<Range> result = std::move(waiters);
        waiters.clear();
        expected = Range {};
        return result;
    }

private:
    bool coverageCurrent = true;
    Range expected;
    QVector<Range> waiters;
};

} // namespace Mattermost
