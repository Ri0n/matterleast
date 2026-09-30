#include "RecentMentionsService.h"

#include <algorithm>
#include <utility>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPointer>
#include <QSet>

#include "Backend.h"
#include "NetworkRequest.h"
#include "QByteArrayCreator.h"
#include "RecentMentionsPolicy.h"

namespace Mattermost {
namespace {

PostRepository::CollectionPage collectionPageFromDocument(
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

RecentMentionsService& RecentMentionsService::instance(Backend& sourceBackend)
{
    static QHash<Backend*, QPointer<RecentMentionsService>> instances;
    QPointer<RecentMentionsService>& service = instances[&sourceBackend];
    if (!service) {
        service = new RecentMentionsService(sourceBackend);
    }
    return *service;
}

RecentMentionsService::RecentMentionsService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);
}

void RecentMentionsService::loadPage(int page,
                                     int perPage,
                                     CollectionCallback callback)
{
    const QString terms = RecentMentionsPolicy::searchTerms(backend.getLoginUser());
    if (terms.isEmpty()) {
        if (callback) {
            PostRepository::CollectionPage result;
            result.success = true;
            callback(result);
        }
        return;
    }

    const int safePage = std::max(0, page);
    const int safePerPage = std::max(1, perPage);
    QJsonObject body {
        {QStringLiteral("terms"), terms},
        {QStringLiteral("is_or_search"), true},
        {QStringLiteral("include_deleted_channels"), true},
        {QStringLiteral("time_zone_offset"),
         QDateTime::currentDateTime().offsetFromUtc()},
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
                    callback(collectionPageFromDocument(
                        status, document, safePerPage));
                }
            }));
}

} // namespace Mattermost
