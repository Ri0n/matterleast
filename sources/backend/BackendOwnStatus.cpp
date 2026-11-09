#include "Backend.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

#include "QByteArrayCreator.h"
#include "CustomStatusValidation.h"
#include "NetworkRequest.h"
#include "types/BackendUser.h"

namespace Mattermost {
namespace {

bool succeeded(const QNetworkReply& reply)
{
    const int code = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return reply.error() == QNetworkReply::NoError && code >= 200 && code < 300;
}

QString failureMessage(const QNetworkReply& reply)
{
    return reply.errorString().isEmpty()
        ? QObject::tr("Server rejected the status change") : reply.errorString();
}

} // namespace

void Backend::reportUserActivity(bool active)
{
    if (isLoggedIn)
        webSocketConnector.reportUserActivity(active);
}

void Backend::setOwnAvailability(const QString& status, StatusUpdateResult callback)
{
    if (status != QLatin1String("online") && status != QLatin1String("away")
        && status != QLatin1String("dnd") && status != QLatin1String("offline")) {
        if (callback) callback(false, tr("Invalid availability value"));
        return;
    }
    if (!storage.loginUser) {
        if (callback) callback(false, tr("Not logged in"));
        return;
    }
    const QString id = storage.loginUser->id;
    NetworkRequest request(QStringLiteral("users/") + id + QStringLiteral("/status"));
    // An unavailable route must not leave the avatar spinning for minutes.
    request.setTransferTimeout(10000);
    const QJsonObject payload {{QStringLiteral("user_id"), id},
                               {QStringLiteral("status"), status}};
    QPointer<Backend> guard(this);
    httpConnector.put(request, payload,
        HttpResponseCallback([guard, id, status, callback](
                                 const QJsonDocument&, const QNetworkReply& reply) {
            if (!guard || !guard->storage.loginUser
                || guard->storage.loginUser->id != id) {
                return;
            }
            if (succeeded(reply)) {
                if (auto* user = guard->storage.getUserById(id)) {
                    if (user->status != status) {
                        user->status = status;
                        emit user->onStatusChanged();
                    }
                }
                if (callback) callback(true, QString());
                return;
            }

            const int code =
                reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (code != 0) {
                // A definitive HTTP rejection is not a connection failure.
                if (callback) callback(false, failureMessage(reply));
                return;
            }

            // A lost or canceled PUT response is ambiguous: the server may
            // already have applied the change. Never blindly replay a write.
            // Read the authoritative state on the new transport instead.
            NetworkRequest check(QStringLiteral("users/") + id
                                 + QStringLiteral("/status"));
            check.setTransferTimeout(10000);
            guard->httpConnector.get(check,
                HttpResponseCallback([guard, id, status, callback](
                                         const QJsonDocument& doc,
                                         const QNetworkReply& checkReply) {
                    if (!guard || !guard->storage.loginUser
                        || guard->storage.loginUser->id != id) {
                        return;
                    }
                    const QString actual = doc.object()
                                               .value(QStringLiteral("status"))
                                               .toString();
                    if (!succeeded(checkReply)
                        || (actual != QLatin1String("online")
                            && actual != QLatin1String("away")
                            && actual != QLatin1String("dnd")
                            && actual != QLatin1String("offline"))) {
                        if (callback) {
                            callback(false, QObject::tr(
                                "Could not verify the availability change after a "
                                "connection interruption. Check the connection and try again."));
                        }
                        return;
                    }
                    if (auto* user = guard->storage.getUserById(id)) {
                        if (user->status != actual) {
                            user->status = actual;
                            emit user->onStatusChanged();
                        }
                    }
                    if (callback) {
                        callback(actual == status,
                                 actual == status ? QString() : QObject::tr(
                                     "The server reports a different availability (%1).")
                                     .arg(actual));
                    }
                }));
        }));
}

void Backend::setOwnCustomStatus(const QString& emoji, const QString& text,
                                 const QString& duration, const QString& expiresAt,
                                 StatusUpdateResult callback)
{
    if (!storage.loginUser) {
        if (callback) callback(false, tr("Not logged in"));
        return;
    }
    if (!hasCustomStatusContent(emoji, text)) {
        if (callback) callback(false, tr("Choose an emoji or enter a status message"));
        return;
    }
    if (text.size() > 100) {
        if (callback) callback(false, tr("Status text must not exceed 100 characters"));
        return;
    }
    const QString id = storage.loginUser->id;
    NetworkRequest request(QStringLiteral("users/") + id + QStringLiteral("/status/custom"));
    request.setTransferTimeout(10000);
    const QString trimmedEmoji = emoji.trimmed();
    QJsonObject payload {{QStringLiteral("emoji"), trimmedEmoji.isEmpty()
                                       ? QStringLiteral("speech_balloon") : trimmedEmoji},
                         {QStringLiteral("text"), text.trimmed()}};
    if (!duration.isEmpty()) payload.insert(QStringLiteral("duration"), duration);
    if (!expiresAt.isEmpty()) payload.insert(QStringLiteral("expires_at"), expiresAt);
    QPointer<Backend> guard(this);
    httpConnector.put(request, payload,
        HttpResponseCallback([guard, id, payload, duration, expiresAt, callback](
                                 const QJsonDocument&, const QNetworkReply& reply) {
            const bool ok = succeeded(reply);
            if (guard && ok) {
                if (auto* user = guard->storage.getUserById(id)) {
                    user->props.customStatus.emoji =
                        payload.value(QStringLiteral("emoji")).toString();
                    user->props.customStatus.text =
                        payload.value(QStringLiteral("text")).toString();
                    user->props.customStatus.duration = duration;
                    user->props.customStatus.expiresAt = expiresAt;
                    emit guard->onOwnCustomStatusChanged();
                }
            }
            if (callback) callback(ok, ok ? QString() : failureMessage(reply));
        }));
}

void Backend::clearOwnCustomStatus(StatusUpdateResult callback)
{
    if (!storage.loginUser) {
        if (callback) callback(false, tr("Not logged in"));
        return;
    }
    const QString id = storage.loginUser->id;
    NetworkRequest request(QStringLiteral("users/") + id + QStringLiteral("/status/custom"));
    request.setTransferTimeout(10000);
    QPointer<Backend> guard(this);
    httpConnector.del(request, QByteArrayCreator(QByteArray()),
        HttpResponseCallback([guard, id, callback](
                                 const QJsonDocument&, const QNetworkReply& reply) {
            const bool ok = succeeded(reply);
            if (guard && ok) {
                if (auto* user = guard->storage.getUserById(id)) {
                    user->props.customStatus = {};
                    emit guard->onOwnCustomStatusChanged();
                }
            }
            if (callback) callback(ok, ok ? QString() : failureMessage(reply));
        }));
}

} // namespace Mattermost
