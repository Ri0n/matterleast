#include "ChannelReferenceService.h"

#include <utility>

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrl>

#include "Backend.h"
#include "HttpResponseCallback.h"
#include "NetworkRequest.h"

namespace Mattermost {

ChannelReferenceService& ChannelReferenceService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<ChannelReferenceService>> instances;
    auto& instance = instances[&backend];
    if (!instance)
        instance = new ChannelReferenceService(backend);
    return *instance;
}

ChannelReferenceService::ChannelReferenceService(Backend& backend)
    : QObject(&backend)
{
}

void ChannelReferenceService::resolve(
    const QString& teamName, const QString& slug, Callback callback)
{
    resolveDetails(teamName, slug, [callback = std::move(callback)](
                       const Details& details) {
        if (callback) callback(details.displayName);
    });
}

void ChannelReferenceService::resolveId(
    const QString& teamName, const QString& slug, IdCallback callback)
{
    resolveDetails(teamName, slug, [callback = std::move(callback)](
                       const Details& details) {
        if (callback) callback(details.id);
    });
}

void ChannelReferenceService::resolveDetails(
    const QString& teamName, const QString& slug, DetailsCallback callback)
{
    if (teamName.isEmpty() || slug.isEmpty()) {
        if (callback) callback({});
        return;
    }

    const QString key = teamName + QLatin1Char('/') + slug;
    const auto cached = _resolved.constFind(key);
    if (cached != _resolved.cend()) {
        if (callback) callback(*cached);
        return;
    }

    auto pending = _pending.find(key);
    if (pending != _pending.end()) {
        pending->push_back(std::move(callback));
        return;
    }
    _pending.insert(key, QVector<DetailsCallback> {std::move(callback)});

    const QString endpoint = QStringLiteral("teams/name/")
        + QString::fromLatin1(QUrl::toPercentEncoding(teamName))
        + QStringLiteral("/channels/name/")
        + QString::fromLatin1(QUrl::toPercentEncoding(slug));
    NetworkRequest request(endpoint);
    QPointer<ChannelReferenceService> guard(this);
    const auto complete = [guard, key](const Details& details, bool definitiveMiss) {
        if (!guard) return;
        // Cache 403/404 misses too: an unknown/private channel reference must
        // not repeatedly trigger identical requests when posts rematerialize.
        if (!details.id.isEmpty() || !details.displayName.isEmpty() || definitiveMiss)
            guard->_resolved.insert(key, details);
        const auto callbacks = guard->_pending.take(key);
        for (const DetailsCallback& current : callbacks)
            if (current) current(details);
    };
    _http.get(request, HttpResponseCallback(
        [guard, slug, complete](QVariant, QByteArray data, const QNetworkReply& reply) {
            if (!guard) return;
            const int status = reply.attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            Details details;
            if (reply.error() == QNetworkReply::NoError
                && status >= 200 && status < 300) {
                const QJsonObject object = QJsonDocument::fromJson(data).object();
                details.id = object.value(QStringLiteral("id")).toString();
                details.displayName =
                    object.value(QStringLiteral("display_name")).toString();
            }

            // Mattermost channel IDs have 26 alphanumeric characters. A
            // permalink can use that identity rather than the team-local slug.
            static const QRegularExpression channelIdExpression(
                QStringLiteral("^[a-z0-9]{26}$"));
            if (status == 404 && channelIdExpression.match(slug).hasMatch()) {
                NetworkRequest byId(QStringLiteral("channels/") + slug);
                guard->_http.get(byId, HttpResponseCallback(
                    [complete](QVariant, QByteArray data, const QNetworkReply& fallback) {
                        const int resultStatus = fallback.attribute(
                            QNetworkRequest::HttpStatusCodeAttribute).toInt();
                        Details fallbackDetails;
                        if (fallback.error() == QNetworkReply::NoError
                            && resultStatus >= 200 && resultStatus < 300) {
                            const QJsonObject object =
                                QJsonDocument::fromJson(data).object();
                            fallbackDetails.id =
                                object.value(QStringLiteral("id")).toString();
                            fallbackDetails.displayName =
                                object.value(QStringLiteral("display_name")).toString();
                        }
                        complete(fallbackDetails,
                                 resultStatus == 403 || resultStatus == 404);
                    }));
                return;
            }
            complete(details, status == 403 || status == 404);
        }));
}

} // namespace Mattermost
