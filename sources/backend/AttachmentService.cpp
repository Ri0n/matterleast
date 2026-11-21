#include "AttachmentService.h"

#include <utility>

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QSaveFile>
#include <QSharedPointer>
#include <QPointer>
#include <QTimer>

#include "Backend.h"
#include "HttpResponseCallback.h"
#include "NetworkRequest.h"
#include "NetworkSessionCookieJar.h"

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
    , httpConnector(sourceBackend.httpTransport())
{
    // The streaming manager is intentionally separate: QSaveFile receives
    // chunks without holding the entire attachment in RAM. Its implicit Qt
    // cookie jar must not retain credentials across login/logout.
    downloadManager.setCookieJar(new NetworkSessionCookieJar(&downloadManager));
}

void AttachmentService::clearSessionState()
{
    localFiles.clear();
    // HTTPConnector's reset invalidates these requests; drop their callbacks
    // so they cannot later be delivered to a different account's widgets.
    pendingCallbacks.clear();

    // Streaming downloads use their own QNetworkAccessManager, not the
    // HTTPConnector. Cancel them before destroying the old session UI, and
    // suppress callbacks owned by this service during teardown.
    QList<QPointer<QNetworkReply>> downloads;
    for (QNetworkReply* reply :
         downloadManager.findChildren<QNetworkReply*>(QString(),
                                                       Qt::FindDirectChildrenOnly)) {
        downloads.append(QPointer<QNetworkReply>(reply));
    }
    for (const QPointer<QNetworkReply>& reply : downloads) {
        if (!reply)
            continue;
        QObject::disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void AttachmentService::rememberLocalFile(const QString& fileId, const QString& path)
{
    const QFileInfo file(path);
    if (fileId.isEmpty() || !file.isFile() || !file.isReadable()) return;

    const QString absolutePath = file.absoluteFilePath();
    auto& entries = localFiles[fileId];
    for (auto it = entries.begin(); it != entries.end();) {
        if (it->path == absolutePath) {
            it = entries.erase(it);
        } else {
            ++it;
        }
    }
    entries.prepend({absolutePath, file.size(), file.lastModified()});
    // Repeated saves of one attachment should not grow the session indefinitely.
    if (entries.size() > 8) entries.resize(8);
}

QString AttachmentService::localFilePath(const QString& fileId, qint64 expectedSize,
                                         const QString& preferredPath)
{
    auto found = localFiles.find(fileId);
    if (found == localFiles.end()) return {};

    const QString preferred = preferredPath.isEmpty()
        ? QString() : QFileInfo(preferredPath).absoluteFilePath();
    auto& entries = found.value();
    QString fallback;
    for (auto it = entries.begin(); it != entries.end();) {
        const QFileInfo file(it->path);
        if (!file.isFile() || !file.isReadable() || file.size() != it->size
            || file.lastModified() != it->modified) {
            it = entries.erase(it);
            continue;
        }
        if (expectedSize < 0 || file.size() == expectedSize) {
            if (preferred.isEmpty() || it->path == preferred) return it->path;
            if (fallback.isEmpty()) fallback = it->path;
        }
        ++it;
    }
    if (entries.isEmpty()) localFiles.erase(found);
    return fallback;
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

    NetworkRequest request(backend.networkSession(), requestPath, true);
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
    NetworkRequest request(backend.networkSession(), requestPath, false);
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


QNetworkReply* AttachmentService::downloadToFile(
    const QString& fileId, const QString& path,
    ProgressCallback progress, CompletionCallback completed)
{
    if (fileId.isEmpty()) {
        if (completed) completed(tr("Missing file ID"));
        return nullptr;
    }

    auto output = QSharedPointer<QSaveFile>::create(path);
    if (!output->open(QIODevice::WriteOnly)) {
        if (completed) completed(output->errorString());
        return nullptr;
    }

    const QString requestPath = QStringLiteral("files/") + fileId;
    NetworkRequest request(backend.networkSession(), requestPath, false);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    QNetworkReply* reply = downloadManager.get(request);
    struct State {
        QByteArray serverError;
        QString writeError;
    };
    auto state = QSharedPointer<State>::create();

    const auto receive = [reply, output, state] {
        const QByteArray chunk = reply->readAll();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status < 200 || status >= 300) {
            if (state->serverError.size() < 16384)
                state->serverError.append(chunk.left(16384 - state->serverError.size()));
            return;
        }
        if (state->writeError.isEmpty() && output->write(chunk) != chunk.size()) {
            state->writeError = output->errorString();
            reply->abort();
        }
    };
    connect(reply, &QIODevice::readyRead, this, receive);
    if (progress) {
        connect(reply, &QNetworkReply::downloadProgress, this,
                [progress](qint64 received, qint64 total) { progress(received, total); });
    }
    connect(reply, &QNetworkReply::finished, this,
            [reply, output, state, receive, completed = std::move(completed), requestPath] {
        receive();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString error = state->writeError;
        if (error.isEmpty() && (reply->error() != QNetworkReply::NoError
                                || status < 200 || status >= 300)) {
            error = reply->errorString();
            const QJsonDocument document = QJsonDocument::fromJson(state->serverError);
            if (document.isObject()) {
                const QJsonObject serverError = document.object();
                const QString message = serverError.value(QStringLiteral("message")).toString();
                const QString id = serverError.value(QStringLiteral("id")).toString();
                if (!message.isEmpty()) error = message;
                if (!id.isEmpty()) error += QStringLiteral(" (%1)").arg(id);
            }
            error = QStringLiteral("HTTP %1: %2").arg(status).arg(error);
        }
        if (error.isEmpty() && !output->commit()) {
            error = output->errorString();
        }
        if (!error.isEmpty()) {
            output->cancelWriting();
            qWarning().noquote() << "ATTACHMENT_DOWNLOAD_FAILED" << requestPath
                                 << "HTTP" << status << error;
        }
        reply->deleteLater();
        if (completed) completed(error);
    });
    return reply;
}

} // namespace Mattermost
