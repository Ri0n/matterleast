#include "LegacyTokenRecovery.h"

#include <QLoggingCategory>
#include <QUrl>
#include <utility>

namespace Mattermost {
namespace {
Q_LOGGING_CATEGORY(legacyTokenLog, "mattermost.migration")
}

QString LegacyTokenRecovery::candidateLoginName(
    const WorkspaceLayout::Account& record)
{
    // The legacy UI constructed this label from the *verified* server host
    // and BackendUser::username; it was not the login identifier that may
    // have been an email address or an outdated settings value.
    const QString host = QUrl(record.serverBaseUrl).host();
    if (host.isEmpty()) return {};
    const QString prefix = host + QStringLiteral(" — ");
    if (!record.name.startsWith(prefix, Qt::CaseInsensitive))
        return {};
    const QString candidate = record.name.mid(prefix.size()).trimmed();
    if (candidate.isEmpty() || candidate == record.loginName
        || candidate.contains(QLatin1Char('/'))
        || candidate.contains(QLatin1Char('@')))
        return {};
    return candidate;
}

void LegacyTokenRecovery::read(
    QObject* context, const WorkspaceLayout::Account& record,
    Callback callback)
{
    const QString server = record.serverBaseUrl;
    const QString currentLogin = record.loginName;
    const QString alternateLogin = candidateLoginName(record);
    SessionTokenStore::read(context, server, currentLogin,
        [context, server, alternateLogin, callback = std::move(callback)](
            SessionTokenStore::ReadStatus status, const QString& token) mutable {
        if (!callback) return;
        if (status != SessionTokenStore::ReadStatus::NotFound
            || alternateLogin.isEmpty()) {
            callback(status, token, false);
            return;
        }
        // A second lookup is safe: only the exact historical server-name
        // display convention is admitted. Neither lookup changes the wallet.
        qCInfo(legacyTokenLog)
            << "Saved key not found, trying the historical account-display login";
        SessionTokenStore::read(context, server, alternateLogin,
            [callback = std::move(callback)](
                SessionTokenStore::ReadStatus alternateStatus,
                const QString& alternateToken) {
            if (!callback) return;
            if (alternateStatus == SessionTokenStore::ReadStatus::Found)
                qCInfo(legacyTokenLog)
                    << "Historical key found; server identity verification required";
            callback(alternateStatus, alternateToken,
                     alternateStatus == SessionTokenStore::ReadStatus::Found);
        });
    });
}

} // namespace Mattermost
