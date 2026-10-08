#pragma once

#include <functional>

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QVector>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;
class QNetworkReply;

class AttachmentService : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(const QByteArray&)>;
    using DownloadCallback = std::function<void(const QByteArray&, const QString&)>;

    static AttachmentService& instance(Backend& backend);

    void retrieveFile(const QString& fileId, Callback callback);
    void downloadFile(const QString& fileId, DownloadCallback callback);
    using ProgressCallback = std::function<void(qint64, qint64)>;
    using CompletionCallback = std::function<void(const QString&)>;
    // Streams successful responses to an atomic destination file. The reply
    // may be aborted by the caller; destruction belongs to this service.
    QNetworkReply* downloadToFile(const QString& fileId, const QString& path,
                                  ProgressCallback progress, CompletionCallback completed);
    void retrievePreview(const QString& fileId, Callback callback);
    void retrieveThumbnail(const QString& fileId, Callback callback);

private:
    explicit AttachmentService(Backend& backend);
    void retrieve(const QString& requestPath, Callback callback);
    void retrieveChecked(const QString& requestPath, DownloadCallback callback);

    Backend& backend;
    HTTPConnector httpConnector;
    QNetworkAccessManager downloadManager;
    QHash<QString, QVector<Callback>> pendingCallbacks;
};

} // namespace Mattermost
