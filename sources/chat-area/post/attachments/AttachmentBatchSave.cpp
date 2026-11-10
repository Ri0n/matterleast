#include "AttachmentBatchSave.h"

#include <utility>

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QNetworkReply>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QTimer>

#include "AttachmentSavePaths.h"
#include "backend/AttachmentService.h"
#include "backend/Backend.h"
#include "options/MLOptions.h"
#include "Settings.h"

namespace Mattermost {

void AttachmentBatchSave::chooseFolderAndSave(
    Backend& backend, std::list<BackendFile> files, QWidget* parent)
{
    if (files.size() < 2) {
        return;
    }
    const QString downloadDir =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString preferred = MLOptions::instance()
        ->optionObject<QString>(DOWNLOAD_LOCATION, downloadDir)->value().toString();

    const QString directory = QFileDialog::getExistingDirectory(
        parent, tr("Save all attachments to…"), preferred);
    if (directory.isEmpty()) {
        return;
    }

    auto* job = new AttachmentBatchSave(backend, std::move(files), directory);
    QTimer::singleShot(0, job, [job] { job->startNext(); });
}

AttachmentBatchSave::AttachmentBatchSave(
    Backend& backend, std::list<BackendFile> files, QString directory)
    : QObject(&backend)
    , _backend(backend)
    , _files(std::move(files))
    , _directory(std::move(directory))
{
    // The progress window and this job outlive a recycled/hidden PostWidget.
    _dialog = new QProgressDialog(
        tr("Preparing attachments…"), tr("Cancel"),
        0, static_cast<int>(_files.size()));
    _dialog->setWindowTitle(tr("Saving attachments"));
    _dialog->setWindowModality(Qt::NonModal);
    _dialog->setMinimumDuration(0);
    _dialog->setAutoClose(false);
    _dialog->setAutoReset(false);
    QObject::connect(_dialog, &QProgressDialog::canceled,
                     this, [this] { cancel(); });
    _dialog->show();
}

AttachmentBatchSave::~AttachmentBatchSave()
{
    _finished = true;
    _cancelled = true;
    if (_reply) {
        _reply->abort();
    }
    if (_dialog) {
        delete _dialog.data();
    }
}

void AttachmentBatchSave::startNext()
{
    if (_finished) {
        return;
    }
    if (_cancelled || _files.empty()) {
        finish();
        return;
    }

    const BackendFile file = _files.front();
    _files.pop_front();
    const QDir folder(_directory);
    const QString target = AttachmentSavePaths::unusedPath(
        folder, file.name, _reservedNames);
    _reservedNames.insert(QFileInfo(target).fileName());

    if (_dialog) {
        _dialog->setLabelText(
            tr("Saving %1 (%2 of %3)…")
                .arg(AttachmentSavePaths::safeName(file.name))
                .arg(_completed + 1)
                .arg(_dialog->maximum()));
    }

    _reply = AttachmentService::instance(_backend).downloadToFile(
        file.id, target,
        {}, [this, file](const QString& error) {
            _reply = nullptr;
            // Some failures (invalid ID, inability to open the target) are
            // synchronous. Defer advancing so failed files cannot recurse.
            QTimer::singleShot(0, this, [this, file, error] {
                if (_finished) {
                    return;
                }
                ++_completed;
                if (error.isEmpty() && !_cancelled) {
                    ++_saved;
                } else if (!_cancelled) {
                    _errors.append(tr("%1: %2").arg(file.name, error));
                }
                if (_dialog) {
                    _dialog->setValue(_completed);
                }
                startNext();
            });
        });
}

void AttachmentBatchSave::cancel()
{
    if (_finished || _cancelled) {
        return;
    }
    _cancelled = true;
    if (_reply) {
        _reply->abort();
    } else {
        finish();
    }
}

void AttachmentBatchSave::finish()
{
    if (_finished) {
        return;
    }
    _finished = true;
    if (_dialog) {
        _dialog->hide();
        _dialog->deleteLater();
        _dialog = nullptr;
    }
    if (!_cancelled && !_errors.isEmpty()) {
        const int failed = _errors.size();
        const QString details = _errors.mid(0, 8).join(QLatin1Char('\n'));
        QMessageBox::warning(
            nullptr, tr("Some attachments could not be saved"),
            tr("Saved %1 file(s); %2 failed.\n%3")
                .arg(_saved).arg(failed).arg(details));
    }
    deleteLater();
}

} // namespace Mattermost
