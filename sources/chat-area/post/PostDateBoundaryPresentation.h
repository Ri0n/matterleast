#pragma once

#include <QDate>
#include <QDateTime>
#include <QLocale>
#include <QString>

#include "backend/types/BackendPost.h"

namespace Mattermost {

/**
 * Date boundaries are presentation state, not synthetic posts. Their
 * classification uses the local calendar date, consistent with timestamps
 * visible in the UI and the author-run grouping policy.
 */
class PostDateBoundaryPresentation
{
public:
    static QDate dateOf(qint64 timestamp)
    {
        return timestamp > 0
            ? QDateTime::fromMSecsSinceEpoch(timestamp).date() : QDate();
    }

    static QDate boundaryDate(const BackendPost& current,
                              const BackendPost* previous,
                              bool firstRow)
    {
        const QDate day = dateOf(static_cast<qint64>(current.create_at));
        if (!day.isValid())
            return {};

        if (firstRow)
            return day;
        // A sparse page does not prove a date change until the immediately
        // preceding post becomes available. A later materialization refresh
        // reclassifies the row without changing post IDs or list indices.
        if (!previous)
            return {};
        const QDate previousDay =
            dateOf(static_cast<qint64>(previous->create_at));
        return previousDay.isValid() && previousDay != day ? day : QDate();
    }

    static QString label(const QDate& date,
                         const QLocale& locale = QLocale::system())
    {
        return date.isValid()
            ? locale.toString(date, QLocale::LongFormat) : QString();
    }
};

} // namespace Mattermost
