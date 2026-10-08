#include "ChannelReferenceService.h"

#include <utility>

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
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
    _http.get(request, HttpResponseCallback(
        [guard, key](QVariant, QByteArray, const QNetworkReply& reply) {
            if (!guard) return;
            QString displayName;
            const int status = reply.attribute(
                QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply.error() == QNetworkReply::NoError
                && status >= 200 && status < 300) {
                const QJsonDocument doc = QJsonDocument::fromJson(reply.readAll());
                if (doc.isObject())
                    displayName = doc.object()
                        .value(QStringLiteral("display_name")).toString();
            }

            if (!displayName.isEmpty())
                guard->_resolved.insert(key, displayName);
            const auto callbacks = guard->_pending.take(key);
            for (const Callback& current : callbacks)
                if (current) current(displayName);
        }));
}

} // namespace Mattermost
