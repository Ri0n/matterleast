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
    const QJsonDocument& document)
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
    return {false, transientHttp || transportFailure};
}

} // namespace Mattermost
