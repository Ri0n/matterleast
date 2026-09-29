#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QString>
#include <QtGlobal>

namespace Mattermost {

/**
 * Presentation-only timestamp formatting matching Mattermost's channel/thread
 * distinction while respecting the operating system's regional conventions.
 */
class PostTimestampPresentation
{
public:
    static QString absoluteTime(qint64 timestamp,
                                const QLocale& locale = QLocale::system())
    {
        return locale.toString(
            QDateTime::fromMSecsSinceEpoch(timestamp).time(),
            QLocale::ShortFormat);
    }

    static QString fullTimestamp(qint64 timestamp,
                                 const QLocale& locale = QLocale::system())
    {
        const QDateTime value = QDateTime::fromMSecsSinceEpoch(timestamp);
        return locale.toString(value.date(), QLocale::LongFormat)
            + QStringLiteral(" ")
            + locale.toString(value.time(), QLocale::LongFormat);
    }

    static QString threadRelativeTime(
        qint64 timestamp,
        qint64 now = QDateTime::currentMSecsSinceEpoch(),
        const QLocale& locale = QLocale::system())
    {
        const qint64 elapsed = qMax<qint64>(0, now - timestamp);

        // Mattermost Timestamp STANDARD_UNITS used by THREADING_TIME:
        // now (<45s), minute (<=59m), hour (<=23.75h), day (<=6d),
        // week (<=3w), then fall back to a locale-formatted date.
        if (elapsed < 45 * Second) {
            return translate("now");
        }
        if (elapsed <= 59 * Minute) {
            const int minutes = qMax(1, qRound(elapsed / double(Minute)));
            return quantity(minutes, "1 minute ago", "%1 minutes ago", locale);
        }
        if (elapsed <= qRound64(23.75 * Hour)) {
            const int hours = qMax(1, qRound(elapsed / double(Hour)));
            return quantity(hours, "1 hour ago", "%1 hours ago", locale);
        }
        if (elapsed <= 6 * Day) {
            const int days = qMax(1, qRound(elapsed / double(Day)));
            return quantity(days, "1 day ago", "%1 days ago", locale);
        }
        if (elapsed <= 3 * Week) {
            const int weeks = qMax(1, qRound(elapsed / double(Week)));
            return quantity(weeks, "1 week ago", "%1 weeks ago", locale);
        }

        return locale.toString(
            QDateTime::fromMSecsSinceEpoch(timestamp).date(),
            QLocale::ShortFormat);
    }

    static int threadRefreshIntervalMs(
        qint64 timestamp,
        qint64 now = QDateTime::currentMSecsSinceEpoch())
    {
        const qint64 elapsed = qMax<qint64>(0, now - timestamp);
        if (elapsed < 45 * Second) {
            return 1000;
        }
        if (elapsed <= 59 * Minute) {
            return 15 * 1000;
        }
        if (elapsed <= qRound64(23.75 * Hour)) {
            return 5 * 60 * 1000;
        }
        return 60 * 60 * 1000;
    }

private:
    static constexpr qint64 Second = 1000;
    static constexpr qint64 Minute = 60 * Second;
    static constexpr qint64 Hour = 60 * Minute;
    static constexpr qint64 Day = 24 * Hour;
    static constexpr qint64 Week = 7 * Day;

    static QString translate(const char* source)
    {
        return QCoreApplication::translate("PostTimestampPresentation", source);
    }

    static QString quantity(int value,
                            const char* singular,
                            const char* plural,
                            const QLocale& locale)
    {
        if (value == 1) {
            return translate(singular);
        }
        return translate(plural).arg(locale.toString(value));
    }
};

} // namespace Mattermost
