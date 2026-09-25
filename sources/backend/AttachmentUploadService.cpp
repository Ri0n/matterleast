#include "AttachmentUploadService.h"

#include <utility>

#include <QUuid>
#include <QPointer>

#include "Backend.h"
#include "Storage.h"
#include "UploadTrace.h"
#include "types/BackendChannel.h"

namespace Mattermost {

AttachmentUploadService& AttachmentUploadService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<AttachmentUploadService>> instances;
    QPointer<AttachmentUploadService>& service = instances[&backend];
    if (!service) {
        service = new AttachmentUploadService(backend);
    }
    return *service;
}

AttachmentUploadService::AttachmentUploadService(Backend& backendInstance)
    : QObject(&backendInstance)
    , backend(backendInstance)
{
}

QString AttachmentUploadService::stage(BackendChannel& channel,
                                       const QString& path,
                                       const QString& requestedUploadId)
{
    if (channel.id.isEmpty() || path.isEmpty()) {
        return {};
    }

    const QString uploadId = requestedUploadId.isEmpty()
        ? QUuid::createUuid().toString(QUuid::WithoutBraces)
        : requestedUploadId;

    const auto existing = uploads.constFind(uploadId);
    if (existing != uploads.cend()
        && existing->channelId == channel.id
        && existing->path == path) {
        return uploadId;
    }

    AttachmentUpload upload;
    upload.id = uploadId;
    upload.channelId = channel.id;
    upload.path = path;
    uploads.insert(uploadId, std::move(upload));
    start(uploadId);
    return uploadId;
}

const AttachmentUpload* AttachmentUploadService::upload(
    const QString& uploadId) const
{
    const auto it = uploads.constFind(uploadId);
    return it == uploads.cend() ? nullptr : &it.value();
}

bool AttachmentUploadService::retry(const QString& uploadId)
{
    auto it = uploads.find(uploadId);
    if (it == uploads.end() || it->state != AttachmentUploadState::Failed) {
        return false;
    }
    start(uploadId);
    return true;
}

void AttachmentUploadService::release(const QString& uploadId)
{
    if (!uploadId.isEmpty()) {
        uploads.remove(uploadId);
    }
}

void AttachmentUploadService::start(const QString& uploadId)
{
    auto it = uploads.find(uploadId);
    if (it == uploads.end()) {
        return;
    }

    BackendChannel* channel =
        backend.getStorage().getChannelById(it->channelId);
    if (!channel) {
        it->state = AttachmentUploadState::Failed;
        it->fileId.clear();
        it->error = tr("Conversation is no longer available");
        ++it->generation;
        emit changed(uploadId);
        return;
    }

    it->state = AttachmentUploadState::Uploading;
    it->fileId.clear();
    it->error.clear();
    const quint64 generation = ++it->generation;
    const QString path = it->path;
    qCInfo(lcUploadTrace).nospace()
        << "ATTACHMENT_UPLOAD_START id=" << uploadId
        << " generation=" << generation
        << " channel=" << it->channelId
        << " file=" << path;
    emit changed(uploadId);

    QPointer<AttachmentUploadService> guard(this);
    backend.uploadFile(
        *channel, path,
        [guard, uploadId, generation](QString fileId, QString errorText) {
            AttachmentUploadService* service = guard.data();
            if (!service) {
                return;
            }
            auto current = service->uploads.find(uploadId);
            if (current == service->uploads.end()
                || current->generation != generation) {
                return;
            }

            current->fileId = fileId;
            current->error = errorText.trimmed();
            current->state = fileId.isEmpty()
                ? AttachmentUploadState::Failed
                : AttachmentUploadState::Ready;
            if (current->state == AttachmentUploadState::Ready) {
                qCInfo(lcUploadTrace).nospace()
                    << "ATTACHMENT_UPLOAD_READY id=" << uploadId
                    << " generation=" << generation
                    << " file=" << current->path
                    << " fileId=" << current->fileId;
            } else {
                qCWarning(lcUploadTrace).nospace()
                    << "ATTACHMENT_UPLOAD_FAILED id=" << uploadId
                    << " generation=" << generation
                    << " file=" << current->path
                    << " error=" << current->error;
            }
            emit service->changed(uploadId);
        });
}

} // namespace Mattermost
