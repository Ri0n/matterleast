#include "Backend.h"

#include <QDateTime>
#include <QJsonObject>
#include <QLocale>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

#include "QByteArrayCreator.h"
#include "CustomStatusValidation.h"
#include "NetworkRequest.h"
#include "types/BackendUser.h"

namespace Mattermost {
namespace {

// The server validates duration and expires_at together; it does not
// calculate the expiry time from a duration name. Match the official
// client's local-calendar semantics, then send an absolute UTC timestamp.
QString customStatusExpiry(const QString& duration, const QString& suppliedExpiry)
{
    if (duration.isEmpty() || !suppliedExpiry.isEmpty())
        return suppliedExpiry;

    const QDateTime now = QDateTime::currentDateTime();
    QDateTime expiry;
    if (duration == QLatin1String("thirty_minutes")) {
        expiry = now.addSecs(30 * 60);
    } else if (duration == QLatin1String("one_hour")) {
        expiry = now.addSecs(60 * 60);
    } else if (duration == QLatin1String("four_hours")) {
        expiry = now.addSecs(4 * 60 * 60);
    } else if (duration == QLatin1String("today")) {
        // Local midnight, not midnight UTC: daylight-saving days vary in length.
        expiry = QDateTime(now.date().addDays(1), QTime(0, 0));
    } else if (duration == QLatin1String("this_week")) {
        // The final day of the user's locale-specific week at 23:59:59.999.
        const int firstDay = int(QLocale().firstDayOfWeek());
        const int daysUntilLastDay =
            (firstDay + 6 - now.date().dayOfWeek() + 7) % 7;
        expiry = QDateTime(now.date().addDays(daysUntilLastDay),
                           QTime(23, 59, 59, 999));
    }
    if (!expiry.isValid())
        return QString();

    // The official client rounds fixed-duration expiry down to a full minute.
    if (duration == QLatin1String("thirty_minutes")
        || duration == QLatin1String("one_hour")
        || duration == QLatin1String("four_hours")) {
        expiry.setTime(QTime(expiry.time().hour(), expiry.time().minute()));
    }
    return expiry.toUTC().toString(Qt::ISODateWithMs);
}

bool succeeded(const QNetworkReply& reply)
{
    const int code = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    return reply.error() == QNetworkReply::NoError && code >= 200 && code < 300;
}

QString failureMessage(const QNetworkReply& reply)
{
    // HTTPConnector aborts outstanding writes when the route changes. Their
    // outcome is unknown, not necessarily a failure: the server might already
    // have committed the status update. Preserve the failure result but leave
    // its message empty so the UI does not notify users about routine cancels.
    if (reply.error() == QNetworkReply::OperationCanceledError) {
        return QString();
    }
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
    const QJsonObject payload {{QStringLiteral("user_id"), id},
                               {QStringLiteral("status"), status}};
    QPointer<Backend> guard(this);
    httpConnector.put(request, payload,
        HttpResponseCallback([guard, id, status, callback](
                                 const QJsonDocument&, const QNetworkReply& reply) {
            const bool ok = succeeded(reply);
            if (guard && ok) {
                if (auto* user = guard->storage.getUserById(id)) {
                    user->status = status;
                    emit user->onStatusChanged();
                }
            }
            if (callback) callback(ok, ok ? QString() : failureMessage(reply));
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
    const QString trimmedEmoji = emoji.trimmed();
    QJsonObject payload {{QStringLiteral("emoji"), trimmedEmoji.isEmpty()
                                       ? QStringLiteral("speech_balloon") : trimmedEmoji},
                         {QStringLiteral("text"), text.trimmed()}};
    const QString resolvedExpiry = customStatusExpiry(duration, expiresAt);
    if (duration == QLatin1String("date_and_time") && resolvedExpiry.isEmpty()) {
        if (callback) callback(false, tr("Choose a date and time for status expiry"));
        return;
    }
    if (!duration.isEmpty()) payload.insert(QStringLiteral("duration"), duration);
    if (!resolvedExpiry.isEmpty()) payload.insert(QStringLiteral("expires_at"), resolvedExpiry);
    QPointer<Backend> guard(this);
    httpConnector.put(request, payload,
        HttpResponseCallback([guard, id, payload, duration, resolvedExpiry, callback](
                                 const QJsonDocument&, const QNetworkReply& reply) {
            const bool ok = succeeded(reply);
            if (guard && ok) {
                if (auto* user = guard->storage.getUserById(id)) {
                    user->props.customStatus.emoji =
                        payload.value(QStringLiteral("emoji")).toString();
                    user->props.customStatus.text =
                        payload.value(QStringLiteral("text")).toString();
                    user->props.customStatus.duration = duration;
                    user->props.customStatus.expiresAt = resolvedExpiry;
                    emit user->onCustomStatusChanged();
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
    QPointer<Backend> guard(this);
    httpConnector.del(request, QByteArrayCreator(QByteArray()),
        HttpResponseCallback([guard, id, callback](
                                 const QJsonDocument&, const QNetworkReply& reply) {
            const bool ok = succeeded(reply);
            if (guard && ok) {
                if (auto* user = guard->storage.getUserById(id)) {
                    user->props.customStatus = {};
                    emit user->onCustomStatusChanged();
                    emit guard->onOwnCustomStatusChanged();
                }
            }
            if (callback) callback(ok, ok ? QString() : failureMessage(reply));
        }));
}

} // namespace Mattermost
