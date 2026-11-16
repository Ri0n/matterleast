#include "AttachmentSaveFile.h"

#include <memory>

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>

#include "AttachmentSavePaths.h"
#include "Settings.h"
#include "backend/AttachmentService.h"
#include "backend/Backend.h"
#include "options/MLOptions.h"

namespace Mattermost {

void AttachmentSaveFile::chooseAndSave(Backend& backend, const QString& fileId,
                                       const QString& fileName, QWidget* parent,
                                       const QString& cachedPath)
{
    // QFileDialog enters a nested event loop. The originating message may be
    // pruned while it is open; snapshot all data and guard the backend first.
    const QString id = fileId;
    const QString name = fileName;
    const QString cached = cachedPath;
    const QPointer<Backend> owner(&backend);
    const QPointer<QWidget> parentGuard(parent);
    const QString defaultDir =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString preferred = MLOptions::instance()
        ->optionObject<QString>(DOWNLOAD_LOCATION, defaultDir)->value().toString();
    const QString destination = QFileDialog::getSaveFileName(
        parent, QObject::tr("Save attachment as…"),
        QDir(preferred).filePath(AttachmentSavePaths::safeName(name)));
    if (destination.isEmpty() || !owner) return;
    saveToPath(*owner, id, destination, parentGuard, cached);
}

void AttachmentSaveFile::saveToPath(Backend& backend, const QString& fileId,
                                    const QString& destination, QWidget* parent,
                                    const QString& cachedPath)
{
    const QPointer<QWidget> feedback(parent);
    const auto reportError = [feedback](const QString& error) {
        if (error.isEmpty()) return;
        if (feedback) {
            QMessageBox::warning(feedback, QObject::tr("Save attachment failed"), error);
        } else {
            qWarning().noquote() << "ATTACHMENT_SAVE_FAILED" << error;
        }
    };

    // Playback already downloaded the original file into a backend-owned
    // temporary cache. Reuse it without a second HTTP request; copy outside
    // the GUI thread and commit atomically to the user's destination.
    if (!cachedPath.isEmpty() && QFileInfo::exists(cachedPath)) {
        auto error = std::make_shared<QString>();
        auto* worker = QThread::create([cachedPath, destination, error] {
            QFile source(cachedPath);
            if (!source.open(QIODevice::ReadOnly)) {
                *error = source.errorString();
                return;
            }
            QSaveFile target(destination);
            if (!target.open(QIODevice::WriteOnly)) {
                *error = target.errorString();
                return;
            }
            while (!source.atEnd()) {
                const QByteArray bytes = source.read(256 * 1024);
                if (bytes.isEmpty() || target.write(bytes) != bytes.size()) {
                    *error = source.error() != QFileDevice::NoError
                        ? source.errorString() : target.errorString();
                    target.cancelWriting();
                    return;
                }
            }
            if (!target.commit()) *error = target.errorString();
        });
        QObject::connect(worker, &QThread::finished, &backend,
                         [error, reportError] { reportError(*error); });
        QObject::connect(worker, &QThread::finished, worker, &QObject::deleteLater);
        worker->start();
        return;
    }

    // QSaveFile inside AttachmentService also prevents partially saved media
    // and handles authentication, redirects and HTTP/server errors.
    AttachmentService::instance(backend).downloadToFile(
        fileId, destination, {}, [reportError](const QString& error) {
            reportError(error);
        });
}

} // namespace Mattermost
