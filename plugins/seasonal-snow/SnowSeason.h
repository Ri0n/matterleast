#pragma once

#include <QDate>
#include <QDateTime>
#include <QTime>
#include <QTimeZone>

namespace MatterLeast::SeasonalSnow {

// Local calendar dates, not UTC. Include the New Year holiday period through
// Orthodox Christmas; there are no animation timers outside this season.
inline bool activeOn(const QDate& date)
{
    return date.isValid()
        && ((date.month() == 12 && date.day() >= 24)
            || (date.month() == 1 && date.day() <= 7));
}

inline int untilNextLocalMidnight(const QDateTime& now)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QDateTime next(now.date().addDays(1), QTime(0, 0),
                         QTimeZone::systemTimeZone());
#else
    const QDateTime next(now.date().addDays(1), QTime(0, 0), Qt::LocalTime);
#endif
    const qint64 remaining = now.msecsTo(next);
    // 26h covers DST transitions; QTimer's interval is an int.
    return static_cast<int>(qBound<qint64>(1000, remaining, 26LL * 3600 * 1000));
}

} // namespace MatterLeast::SeasonalSnow
