#include "PostRepository.h"

#include <algorithm>
#include <utility>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSet>
#include <QTimeZone>

#include "Backend.h"
#include "NetworkRequest.h"
#include "QByteArrayCreator.h"

namespace Mattermost {
namespace {

int currentUserTimeZoneOffset(const BackendUser& user)
{
    const QString zoneName = user.timezone.useAutomaticTimezone
        ? user.timezone.automaticTimezone
        : user.timezone.manualTimezone;
    if (!zoneName.isEmpty()) {
        const QTimeZone zone(zoneName.toUtf8());
        if (zone.isValid()) {
            return zone.offsetFromUtc(QDateTime::currentDateTimeUtc());
        }
    }

    return QDateTime::currentDateTime().offsetFromUtc();
}

PostRepository::CollectionPage recentMentionsPageFromDocument(
    QVariant status, const QJsonDocument& document, int perPage)
{
    PostRepository::CollectionPage result;
    if (status.toInt() != QNetworkReply::NoError || !document.isObject()) {
        return result;
    }

    const QJsonObject root = document.object();
    const QJsonObject objects = root.value(QStringLiteral("posts")).toObject();
    const QJsonArray order = root.value(QStringLiteral("order")).toArray();
    QSet<QString> seen;

    for (const QJsonValue& value : order) {
        const QString id = value.toString();
        const QJsonValue postValue = objects.value(id);
        if (id.isEmpty() || seen.contains(id) || !postValue.isObject()) {
            continue;
        }
        seen.insert(id);
        result.posts.push_back(postValue.toObject());
    }

    if (order.isEmpty()) {
        for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
            if (!it->isObject() || seen.contains(it.key())) {
                continue;
            }
            seen.insert(it.key());
            result.posts.push_back(it->toObject());
        }
    }

    const int returnedCount = order.isEmpty()
        ? static_cast<int>(result.posts.size())
        : order.size();
    result.completeResultSet = perPage > 0 && returnedCount > perPage;
    result.hasMore = !result.completeResultSet
        && perPage > 0 && returnedCount >= perPage;
    result.success = true;
    return result;
}

} // namespace

void PostRepository::searchRecentMentions(const QString& terms,
                                          int page,
                                          int perPage,
                                          CollectionCallback callback)
{
    const QString query = terms.trimmed();
    if (query.isEmpty()) {
        if (callback) {
            CollectionPage empty;
            empty.success = true;
            callback(empty);
        }
        return;
    }

    const int safePage = std::max(0, page);
    const int safePerPage = std::max(1, perPage);
    QJsonObject body {
        {QStringLiteral("terms"), query},
        {QStringLiteral("is_or_search"), true},
        {QStringLiteral("include_deleted_channels"), true},
        {QStringLiteral("time_zone_offset"),
         currentUserTimeZoneOffset(backend.getLoginUser())},
        {QStringLiteral("page"), safePage},
        {QStringLiteral("per_page"), safePerPage},
    };

    NetworkRequest request(QStringLiteral("posts/search"));
    httpConnector.post(
        request,
        QByteArrayCreator(body),
        HttpResponseCallback(
            [safePerPage, callback = std::move(callback)](
                QVariant status, const QJsonDocument& document) mutable {
                if (callback) {
                    callback(recentMentionsPageFromDocument(
                        status, document, safePerPage));
                }
            }));
}

} // namespace Mattermost
