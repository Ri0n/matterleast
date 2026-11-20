#pragma once

#include <functional>

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QVector>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;
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
    // A successfully saved attachment is reusable across virtualized message widgets.
    // Only user-owned destinations are recorded; missing/modified files are
    // rejected at lookup, and entries are scoped to this backend session.
    void rememberLocalFile(const QString& fileId, const QString& path);
    // preferredPath is used when a media decoder reopens an earlier saved
    // location after another copy of the same attachment was saved.
    QString localFilePath(const QString& fileId, qint64 expectedSize = -1,
                          const QString& preferredPath = {});
    void retrievePreview(const QString& fileId, Callback callback);
    void retrieveThumbnail(const QString& fileId, Callback callback);

private:
    explicit AttachmentService(Backend& backend);
    void retrieve(const QString& requestPath, Callback callback);
    void retrieveChecked(const QString& requestPath, DownloadCallback callback);

    Backend& backend;
    HTTPConnector& httpConnector;
    QNetworkAccessManager downloadManager;
    struct LocalFile {
        QString path;
        qint64 size = 0;
        QDateTime modified;
    };
    QHash<QString, QVector<LocalFile>> localFiles;
    QHash<QString, QVector<Callback>> pendingCallbacks;
};

} // namespace Mattermost
