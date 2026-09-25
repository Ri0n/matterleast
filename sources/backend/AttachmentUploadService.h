#pragma once

#include <QHash>
#include <QObject>
#include <QString>

namespace Mattermost {

class Backend;
class BackendChannel;

enum class AttachmentUploadState {
    Uploading,
    Ready,
    Failed,
};

struct AttachmentUpload
{
    QString id;
    QString channelId;
    QString path;
    QString fileId;
    QString error;
    qint64 bytesSent = 0;
    qint64 bytesTotal = 0;
    int progressPercent = -1;
    AttachmentUploadState state = AttachmentUploadState::Uploading;
    quint64 generation = 0;
};

/**
 * Backend-scoped owner for attachment uploads.
 *
 * Composer widgets may stage files before Send for latency hiding, but the
 * upload lifetime is not tied to the composer. PendingPostService can retain
 * the same upload ID after the composer has handed the outgoing operation to
 * the outbox.
 */
class AttachmentUploadService final : public QObject
{
    Q_OBJECT
public:
    static AttachmentUploadService& instance(Backend& backend);

    QString stage(BackendChannel& channel,
                  const QString& path,
                  const QString& uploadId = QString());
    const AttachmentUpload* upload(const QString& uploadId) const;
    bool retry(const QString& uploadId);
    void release(const QString& uploadId);

signals:
    void changed(const QString& uploadId);
    void progressChanged(const QString& uploadId);

private:
    explicit AttachmentUploadService(Backend& backend);
    void start(const QString& uploadId);

    Backend& backend;
    QHash<QString, AttachmentUpload> uploads;
};

} // namespace Mattermost
