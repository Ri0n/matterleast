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
    if (offeredId.startsWith(QStringLiteral("nightly-"))) {
        if (installedId.startsWith(QStringLiteral("nightly-")))
            return offeredId <= installedId;
        if (installedId.startsWith(QStringLiteral("ci-"))) {
            // A CI package may have been built after the last published
            // nightly even though both advertise the same numeric version.
            // Both IDs use UTC YYYYMMDD-HHMMSS, so compare timestamps rather
            // than arbitrary commit hashes or the "ci"/"nightly" prefixes.
            return offeredId.mid(8, 15) <= installedId.mid(3, 15);
        }
    }
    return false;
}

} // namespace Mattermost
