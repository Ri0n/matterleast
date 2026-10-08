#include "AttachmentService.h"

#include <utility>

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QTimer>

#include "Backend.h"
#include "HttpResponseCallback.h"
#include "NetworkRequest.h"

namespace Mattermost {

AttachmentService& AttachmentService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<AttachmentService>> instances;
    QPointer<AttachmentService>& service = instances[&backend];
    if (!service) {
        service = new AttachmentService(backend);
    }
    return *service;
}

AttachmentService::AttachmentService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);
}

void AttachmentService::retrieveFile(const QString& fileId, Callback callback)
{
    if (fileId.isEmpty()) {
        QTimer::singleShot(0, this, [callback = std::move(callback)] {
            if (callback) {
                callback(QByteArray());
            }
        });
        return;
    }

    retrieve(QStringLiteral("files/") + fileId, std::move(callback));
}

void AttachmentService::downloadFile(const QString& fileId, DownloadCallback callback)
{
    if (fileId.isEmpty()) {
        if (callback) callback({}, tr("Missing file ID"));
        return;
    }
    retrieveChecked(QStringLiteral("files/") + fileId, std::move(callback));
}

void AttachmentService::retrievePreview(const QString& fileId, Callback callback)
{
    if (fileId.isEmpty()) {
        QTimer::singleShot(0, this, [callback = std::move(callback)] {
            if (callback) {
                callback(QByteArray());
            }
        });
        return;
    }

    retrieve(QStringLiteral("files/") + fileId + QStringLiteral("/preview"),
             std::move(callback));
}

void AttachmentService::retrieveThumbnail(const QString& fileId, Callback callback)
{
    if (fileId.isEmpty()) {
        QTimer::singleShot(0, this, [callback = std::move(callback)] {
            if (callback) {
                callback(QByteArray());
            }
        });
        return;
    }

    retrieve(QStringLiteral("files/") + fileId + QStringLiteral("/thumbnail"),
             std::move(callback));
}

void AttachmentService::retrieve(const QString& requestPath, Callback callback)
{
    auto pending = pendingCallbacks.find(requestPath);
    if (pending != pendingCallbacks.end()) {
        pending->push_back(std::move(callback));
        return;
    }

    pendingCallbacks.insert(
        requestPath, QVector<Callback> {std::move(callback)});

    NetworkRequest request(requestPath, true);
    request.setPriority(QNetworkRequest::LowPriority);
    request.setAttribute(QNetworkRequest::BackgroundRequestAttribute, true);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::PreferCache);

    httpConnector.get(request, HttpResponseCallback(
        [this, requestPath](QVariant, QByteArray data, const QNetworkReply& reply) {
            const int httpStatus = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply.error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300) {
                data.clear();
            }
            const QVector<Callback> callbacks =
                pendingCallbacks.take(requestPath);
            for (const Callback& current : callbacks) {
                if (current) {
                    current(data);
                }
            }
        }));
}

void AttachmentService::retrieveChecked(const QString& requestPath, DownloadCallback callback)
{
    NetworkRequest request(requestPath, false);
    request.setPriority(QNetworkRequest::LowPriority);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    httpConnector.get(request, HttpResponseCallback(
        [callback = std::move(callback), requestPath](QVariant, QByteArray data,
                                                       const QNetworkReply& reply) {
            const int httpStatus = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (reply.error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300) {
                QString message = reply.errorString();
                const QJsonDocument json = QJsonDocument::fromJson(data);
                if (json.isObject()) {
                    const QJsonObject error = json.object();
                    const QString serverMessage = error.value(QStringLiteral("message")).toString();
                    const QString errorId = error.value(QStringLiteral("id")).toString();
                    if (!serverMessage.isEmpty()) message = serverMessage;
                    if (!errorId.isEmpty()) message += QStringLiteral(" (%1)").arg(errorId);
                }
                qWarning().noquote() << "ATTACHMENT_DOWNLOAD_FAILED"
                                     << requestPath << "HTTP" << httpStatus << message;
                if (callback) callback({}, QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(message));
                return;
            }
            if (callback) callback(data, {});
        }));
}

} // namespace Mattermost
