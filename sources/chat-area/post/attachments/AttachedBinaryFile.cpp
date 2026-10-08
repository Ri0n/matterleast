/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QMessageBox>
#include <QPainter>
#include <QPalette>
#include <QPaintEvent>
#include <QPointer>
#include <QStandardPaths>
#include <QClipboard>
#include <QGuiApplication>
#include <QProcess>
#include <QStyle>
#include <QToolButton>
#include <QHBoxLayout>
#include <QUrl>
#include <QSaveFile>
#include <QNetworkReply>

#include "Settings.h"
#include "AttachedBinaryFile.h"
#include "AttachmentPresentation.h"
#include "ui_AttachedBinaryFile.h"
#include "backend/AttachmentService.h"
#include "backend/types/BackendFile.h"
#include "config/Config.h"
#include "options/MLOptions.h"

namespace Mattermost {

AttachedBinaryFile::AttachedBinaryFile(Backend& backend, const BackendFile& file, QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::AttachedBinaryFile)
{
    ui->setupUi(this);
    ui->fileNameLabel->setMaximumHeight(QWIDGETSIZE_MAX);
    ui->fileTypeLabel->setMaximumHeight(QWIDGETSIZE_MAX);
    ui->fileSizeLabel->setMaximumHeight(QWIDGETSIZE_MAX);
    ui->fileNameLabel->setText("File: " + file.name);
    ui->downloadedLabel->clear();
    ui->downloadedLabel->hide();
    ui->fileNameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ui->fileNameLabel->setWordWrap(true);
    ui->fileNameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ui->fileTypeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ui->fileSizeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ui->downloadedLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ui->downloadedLabel->setWordWrap(true);
    ui->downloadedLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    ui->downloadedLabel->setTextFormat(Qt::PlainText);
    ui->downloadButton->setText({});
    ui->downloadButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    ui->downloadButton->setToolTip(tr("Download file"));
    ui->downloadButton->setFixedSize(30, 30);
    ui->openButton->setText({});
    ui->openButton->setIcon(style()->standardIcon(QStyle::SP_DialogOpenButton));
    ui->openButton->setToolTip(tr("Open file"));
    ui->openButton->setFixedSize(30, 30);

    auto* revealButton = new QToolButton(this);
    revealButton->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    revealButton->setToolTip(tr("Show in file manager"));
    revealButton->setFixedSize(30, 30);
    revealButton->setEnabled(false);
    ui->horizontalLayout_3->insertWidget(2, revealButton);
    connect(revealButton, &QToolButton::clicked, this, [this] {
        if (downloadedPath.isEmpty()) return;
        const QString absolutePath = QFileInfo(downloadedPath).absoluteFilePath();
#ifdef Q_OS_WIN
        if (QProcess::startDetached(QStringLiteral("explorer.exe"),
                                    {QStringLiteral("/select,") + QDir::toNativeSeparators(absolutePath)}))
            return;
#elif defined(Q_OS_MACOS)
        if (QProcess::startDetached(QStringLiteral("open"),
                                    {QStringLiteral("-R"), absolutePath}))
            return;
#else
        // Freedesktop FileManager1 asks the desktop file manager to highlight the file.
        const QString fileUri = QUrl::fromLocalFile(absolutePath).toString(QUrl::FullyEncoded);
        if (QProcess::startDetached(QStringLiteral("dbus-send"),
                {QStringLiteral("--session"), QStringLiteral("--dest=org.freedesktop.FileManager1"),
                 QStringLiteral("--type=method_call"),
                 QStringLiteral("/org/freedesktop/FileManager1"),
                 QStringLiteral("org.freedesktop.FileManager1.ShowItems"),
                 QStringLiteral("array:string:") + fileUri,
                 QStringLiteral("string:")}))
            return;
#endif
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(absolutePath).absolutePath()));
    });
    auto* cancelButton = new QToolButton(this);
    cancelButton->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));
    cancelButton->setToolTip(tr("Cancel download"));
    cancelButton->setFixedSize(30, 30);
    cancelButton->hide();
    ui->horizontalLayout_3->insertWidget(3, cancelButton);
    connect(cancelButton, &QToolButton::clicked, this, [this] {
        if (_downloadReply) _downloadReply->abort();
    });


    static QLocale locale = QLocale::system();
    ui->fileSizeLabel->setText(
        "Size: " + locale.formattedDataSize(file.size, 2, QLocale::DataSizeTraditionalFormat));

    setFileMimeIcon(file.name);

    const QString fileId = file.id;
    const QString fileName = file.name;
    const uint64_t fileSize = file.size;

    connect(ui->downloadButton, &QPushButton::clicked, this,
            [this, &backend, fileId, fileName, fileSize, cancelButton, revealButton] {
        auto* options = MLOptions::instance();
        const QString defaultDownloadDir =
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const QDir downloadDir(
            options->optionObject<QString>(DOWNLOAD_LOCATION, defaultDownloadDir)
                ->value().toString());
        const bool askForLocation =
            options->optionObject<bool>(DOWNLOAD_ASK, DOWNLOAD_ASK_DEFAULT)
                ->value().toBool();

        QString fileDestination = downloadDir.filePath(fileName);
        if (askForLocation) {
            fileDestination = QFileDialog::getSaveFileName(
                this,
                tr("Save file as - MatterLeast"),
                fileDestination);
            if (fileDestination.isEmpty()) {
                return;
            }
        } else {
            const QFileInfo fileInfo(fileDestination);
            if (fileInfo.isFile()
                && static_cast<uint64_t>(fileInfo.size()) == fileSize) {
                QMessageBox msgBox(
                    QMessageBox::Question,
                    "File exists - MatterLeast",
                    "The file '" + fileName + "' is already downloaded to \n'"
                        + fileInfo.absolutePath() + "'",
                    QMessageBox::NoButton,
                    this);
                msgBox.setInformativeText("Please choose:");
                QPushButton* downloadAgainButton =
                    msgBox.addButton("Download Again", QMessageBox::AcceptRole);
                QPushButton* openButton =
                    msgBox.addButton("Open File", QMessageBox::AcceptRole);
                msgBox.setStandardButtons(QMessageBox::Cancel);
                msgBox.setDefaultButton(QMessageBox::Cancel);
                msgBox.exec();

                if (msgBox.clickedButton() == msgBox.button(QMessageBox::Cancel)) {
                    return;
                }
                if (msgBox.clickedButton() == openButton) {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(fileDestination));
                    return;
                }
                if (msgBox.clickedButton() != downloadAgainButton) {
                    return;
                }
            }
        }

        if (_downloadReply) return;
        ui->downloadButton->setDisabled(true);
        ui->openButton->setDisabled(true);
        cancelButton->show();
        ui->downloadedLabel->setText(tr("Downloading…"));
        ui->downloadedLabel->show();
        QPointer<AttachedBinaryFile> self(this);
        _downloadReply = AttachmentService::instance(backend).downloadToFile(
            fileId, fileDestination,
            [self](qint64 received, qint64 total) {
                if (!self) return;
                if (total > 0)
                    self->ui->downloadedLabel->setText(
                        self->tr("Downloading: %1 / %2")
                            .arg(QLocale::system().formattedDataSize(received))
                            .arg(QLocale::system().formattedDataSize(total)));
            },
            [self, fileDestination, cancelButton, revealButton](const QString& error) {
                if (!self) return;
                self->_downloadReply = nullptr;
                self->ui->downloadButton->setEnabled(true);
                self->ui->openButton->setEnabled(true);
                cancelButton->hide();
                if (!error.isEmpty()) {
                    self->ui->downloadedLabel->setText(self->tr("Download failed: %1").arg(error));
                    emit self->dimensionsChanged();
                    return;
                }
                self->downloadedPath = QFileInfo(fileDestination).absoluteFilePath();
                self->ui->downloadedLabel->setText(self->downloadedPath);
                self->ui->downloadedLabel->setToolTip(self->tr("Select and copy the saved file path"));
                revealButton->setEnabled(true);
                emit self->dimensionsChanged();
            });
    });

    connect(ui->openButton, &QPushButton::clicked, this,
            [this, &backend, fileId, fileName, cancelButton] {
        if (!downloadedPath.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(downloadedPath));
            return;
        }

        if (_downloadReply) return;
        QString tmpName(fileName);
        const int dot = tmpName.lastIndexOf(QLatin1Char('.'));
        tmpName.insert(dot < 0 ? tmpName.size() : dot, QStringLiteral("XXXXXX"));
        tempFile.setFileTemplate(Config::tempDirectory().filePath(tmpName));
        if (!tempFile.open()) {
            QMessageBox::warning(this, tr("Open failed"), tempFile.errorString());
            return;
        }
        tempFile.close();
        ui->openButton->setEnabled(false);
        cancelButton->show();
        ui->downloadedLabel->show();
        QPointer<AttachedBinaryFile> self(this);
        _downloadReply = AttachmentService::instance(backend).downloadToFile(
            fileId, tempFile.fileName(),
            [self](qint64 received, qint64 total) {
                if (self && total > 0)
                    self->ui->downloadedLabel->setText(
                        self->tr("Downloading: %1 / %2")
                            .arg(QLocale::system().formattedDataSize(received))
                            .arg(QLocale::system().formattedDataSize(total)));
            },
            [self, cancelButton](const QString& error) {
                if (!self) return;
                self->_downloadReply = nullptr;
                self->ui->openButton->setEnabled(true);
                cancelButton->hide();
                if (!error.isEmpty()) {
                    self->ui->downloadedLabel->setText(self->tr("Download failed: %1").arg(error));
                    emit self->dimensionsChanged();
                    return;
                }
                self->ui->downloadedLabel->hide();
                QDesktopServices::openUrl(QUrl::fromLocalFile(self->tempFile.fileName()));
            });
    });
}

AttachedBinaryFile::~AttachedBinaryFile()
{
    if (_downloadReply) _downloadReply->abort();
    delete ui;
}

void AttachedBinaryFile::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);

    QPainter painter(this);
    painter.fillRect(QRect(0, 0, 3, height()),
                     palette().color(QPalette::Highlight));
}

void AttachedBinaryFile::setFileMimeIcon(const QString& filename)
{
    const auto presentation = AttachmentPresentation::describeFile(filename);
    ui->fileTypeLabel->setText("Type: " + presentation.mimeTypeName);

    if (!presentation.icon.isNull()) {
        const QPixmap pixmap = presentation.icon.pixmap(QSize(64, 64));
        ui->fileIcon->setPixmap(pixmap);
        ui->fileIcon->setFixedSize(pixmap.size());
    }
}

} /* namespace Mattermost */
