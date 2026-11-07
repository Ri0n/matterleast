#pragma once

#include <QDebug>
#include <QString>

namespace Mattermost {

// Qt's "time process" uses a monotonic clock and prints seconds with exactly
// three fractional digits (millisecond precision) on both Qt 5 and Qt 6.
// Prefer Qt's native message formatting over installing a message handler:
// this keeps the platform's existing stderr/debugger output, logging category
// rules, and QT_MESSAGE_PATTERN environment override intact.
inline void configureApplicationLogging()
{
    qSetMessagePattern(QStringLiteral(
        "[%{time process}s] %{if-category}%{category}: %{endif}%{message}"));
}

} // namespace Mattermost
