#pragma once

#include <QString>
#include <QVersionNumber>

namespace Mattermost {

// Compare both the numeric release version and the immutable install identity.
// Nightly builds share a numeric version, but each buildId is distinct.
inline bool windowsUpdateAlreadyCurrentOrOlder(
    const QString& channel, const QString& installedId,
    const QString& offeredId, const QVersionNumber& running,
    const QVersionNumber& offered)
{
    if (installedId == offeredId)
        return true;
    if (!running.isNull() && QVersionNumber::compare(offered, running) < 0)
        return true;
    if (channel == QLatin1String("stable"))
        return !running.isNull() && QVersionNumber::compare(offered, running) <= 0;
    return installedId.startsWith(QStringLiteral("nightly-"))
        && offeredId.startsWith(QStringLiteral("nightly-"))
        && offeredId <= installedId;
}

} // namespace Mattermost
