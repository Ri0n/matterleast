#pragma once

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QString>

namespace Mattermost {

struct LoginResponseValidation {
    bool accepted = false;
    bool retryable = false;
};

// A saved token is *not* proof that /users/me succeeded. In particular,
// QJsonDocument::object() returns an empty object after an offline response.
inline LoginResponseValidation validateLoginResponse(
    QNetworkReply::NetworkError error,
    int httpStatus,
    const QJsonDocument& document,
    bool savedToken = false)
{
    const bool validIdentity = document.isObject()
        && !document.object().value(QStringLiteral("id")).toString().isEmpty();
    if (error == QNetworkReply::NoError && httpStatus >= 200 && httpStatus < 300
        && validIdentity) {
        return {true, false};
    }

    // A lost route, timeout or transient server failure may recover without
    // user input. Expired/invalid credentials are never retried indefinitely.
    const bool transientHttp = httpStatus == 408 || httpStatus == 429
        || httpStatus >= 500;
    const bool transportFailure = httpStatus == 0
        && error != QNetworkReply::NoError;

    // Outside a corporate VPN the proxy/filter may generate 403/404 even
    // though the saved session is valid. Those statuses cannot prove token
    // expiry; keep retrying rather than replacing the login UI with a prompt.
    // A 401 is authoritative only with a recognizable Mattermost API error,
    // not an HTML login page or a generic JSON response from a gateway.
    const QJsonObject body = document.isObject() ? document.object()
                                                 : QJsonObject();
    const bool mattermostAuthError = httpStatus == 401
        && body.value(QStringLiteral("status_code")).toInt() == 401
        && body.value(QStringLiteral("id")).toString()
               .startsWith(QStringLiteral("api."));
    const bool ambiguousGateway = savedToken
        && (httpStatus == 403 || httpStatus == 404
            || (httpStatus == 401 && !mattermostAuthError));
    return {false, transientHttp || transportFailure || ambiguousGateway};
}

} // namespace Mattermost
