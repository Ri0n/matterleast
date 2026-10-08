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
    if (teamName.isEmpty() || slug.isEmpty()) {
        if (callback) callback({});
        return;
    }

    const QString key = teamName + QLatin1Char('/') + slug;
    auto cached = _resolved.constFind(key);
    if (cached != _resolved.cend()) {
        if (callback) callback(*cached);
        return;
    }

    auto pending = _pending.find(key);
    if (pending != _pending.end()) {
        pending->push_back(std::move(callback));
        return;
    }
    _pending.insert(key, QVector<Callback> {std::move(callback)});

    const QString endpoint = QStringLiteral("teams/name/")
        + QString::fromLatin1(QUrl::toPercentEncoding(teamName))
        + QStringLiteral("/channels/name/")
        + QString::fromLatin1(QUrl::toPercentEncoding(slug));
    NetworkRequest request(endpoint);
    QPointer<ChannelReferenceService> guard(this);
    const auto complete = [guard, key](const QString& displayName, bool definitiveMiss) {
        if (!guard) return;
        // Cache 403/404 misses too: an unknown/private channel mention must not
        // repeatedly trigger identical HTTP requests when posts rematerialize.
        if (!displayName.isEmpty() || definitiveMiss)
            guard->_resolved.insert(key, displayName);
        const auto callbacks = guard->_pending.take(key);
        for (const Callback& current : callbacks)
            if (current) current(displayName);
    };
    _http.get(request, HttpResponseCallback(
        [guard, slug, complete](QVariant, QByteArray data, const QNetworkReply& reply) {
            if (!guard) return;
            const int status = reply.attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString displayName;
            if (reply.error() == QNetworkReply::NoError
                && status >= 200 && status < 300) {
                displayName = QJsonDocument::fromJson(data).object()
                    .value(QStringLiteral("display_name")).toString();
            }
            // Mattermost channel IDs have 26 alphanumeric characters. A
            // permalink can use that identity rather than the team-local slug.
            static const QRegularExpression channelIdExpression(
                QStringLiteral("^[a-z0-9]{26}$"));
            if (status == 404
                && channelIdExpression.match(slug).hasMatch()) {
                NetworkRequest byId(QStringLiteral("channels/") + slug);
                guard->_http.get(byId, HttpResponseCallback(
                    [complete](QVariant, QByteArray data, const QNetworkReply& fallback) {
                        const int resultStatus = fallback.attribute(
                            QNetworkRequest::HttpStatusCodeAttribute).toInt();
                        QString name;
                        if (fallback.error() == QNetworkReply::NoError
                            && resultStatus >= 200 && resultStatus < 300)
                            name = QJsonDocument::fromJson(data).object()
                                .value(QStringLiteral("display_name")).toString();
                        complete(name, resultStatus == 403 || resultStatus == 404);
                    }));
                return;
            }
            complete(displayName, status == 403 || status == 404);
        }));
}

} // namespace Mattermost
