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

#include "FilePreview.h"
#include "ui_FilePreview.h"

#include <algorithm>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QMenu>
#include <QResizeEvent>
#include <QScreen>
#include <QSizePolicy>
#include <QStandardPaths>
#if QT_VERSION < QT_VERSION_CHECK(6,0,0)
#include <QDesktopWidget>
#endif

namespace Mattermost  {

FilePreview::FilePreview(const FilePreviewData& file, QWidget* parent)
    : FilePreview(
          QImage::fromData(file.fileContents),
          file.fileName,
          file.fileAuthor,
          parent,
          [contents = file.fileContents](const QString& destination) {
              QFile output(destination);
              if (!output.open(QIODevice::WriteOnly)) {
                  qWarning() << "Cannot save image to" << destination << ":"
                             << output.errorString();
                  return;
              }
              output.write(contents);
          })
{
}

FilePreview::FilePreview(const QImage& image,
                         const QString& fileName,
                         const QString& fileAuthor,
                         QWidget* parent,
                         SaveCallback saveCallback)
    : QDialog(parent)
    , ui(new Ui::FilePreview)
    , fileName(fileName)
    , saveCallback(std::move(saveCallback))
{
    ui->setupUi(this);
    setWindowTitle(fileName + " [" + fileAuthor + "] - Mattermost");

    sourcePixmap = QPixmap::fromImage(image);
    // Keep only the native window-manager decoration. The designer file has
    // legacy Box frames around the image area which become visible as a white
    // border once the pixmap is scaled explicitly.
    ui->frame->setFrameShape(QFrame::NoFrame);
    ui->fileContents->setFrameShape(QFrame::NoFrame);
    ui->fileContents->setScaledContents(false);
    ui->fileContents->setAlignment(Qt::AlignCenter);
    ui->fileContents->setMinimumSize(1, 1);
    ui->fileContents->setSizePolicy(QSizePolicy::Expanding,
                                    QSizePolicy::Expanding);
    ui->fileContents->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->fileContents, &QWidget::customContextMenuRequested,
            this, &FilePreview::showContextMenu);

    if (layout()) {
        layout()->activate();
    }

    // Measure the non-image part of the dialog once. Resize handling then uses
    // the requested dialog geometry directly instead of feeding the current
    // QLabel/viewport size back into the next layout pass.
    imageChromeSize = QSize(
        std::max(0, width() - ui->fileContents->width()),
        std::max(0, height() - ui->fileContents->height()));

    const QSize imageArea = initialImageAreaSize();
    resize(imageArea + imageChromeSize);
    updateDisplayedPixmap(imageArea);
}

FilePreview::~FilePreview()
{
    delete ui;
}

QSize FilePreview::fitImageSize(const QSize& availableSize,
                                bool allowUpscale) const
{
    if (sourcePixmap.isNull() || availableSize.isEmpty()) {
        return QSize(1, 1);
    }

    QSize fitted = sourcePixmap.size();
    fitted.scale(availableSize.expandedTo(QSize(1, 1)), Qt::KeepAspectRatio);

    // Initial presentation should never blow a small source up to screen size.
    // Once the user explicitly enlarges the window, however, use the whole
    // available area just like an ordinary image viewer.
    if (!allowUpscale
        && (fitted.width() > sourcePixmap.width()
            || fitted.height() > sourcePixmap.height())) {
        fitted = sourcePixmap.size();
    }

    return fitted.expandedTo(QSize(1, 1));
}

QSize FilePreview::initialImageAreaSize() const
{
#if QT_VERSION < QT_VERSION_CHECK(6,0,0)
    const QRect screenGeometry = QApplication::desktop()->screenGeometry(
        const_cast<FilePreview*>(this));
#else
    QScreen* targetScreen = screen();
    if (!targetScreen) {
        targetScreen = QGuiApplication::primaryScreen();
    }
    const QRect screenGeometry = targetScreen
        ? targetScreen->availableGeometry()
        : QRect(0, 0, 1024, 768);
#endif

    const QSize bounds(
        std::max(240, qRound(screenGeometry.width() * 0.9)),
        std::max(180, qRound(screenGeometry.height() * 0.8)));
    return fitImageSize(bounds, false);
}

QSize FilePreview::imageAreaForDialogSize(const QSize& dialogSize) const
{
    return QSize(
               std::max(1, dialogSize.width() - imageChromeSize.width()),
               std::max(1, dialogSize.height() - imageChromeSize.height()))
        .expandedTo(QSize(1, 1));
}

void FilePreview::updateDisplayedPixmap(const QSize& availableSize)
{
    if (!ui || !ui->fileContents || sourcePixmap.isNull()) {
        return;
    }

    const QSize fitted = fitImageSize(availableSize, true);
    ui->fileContents->setPixmap(
        sourcePixmap.scaled(
            fitted,
            Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
}

void FilePreview::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if (!event) {
        return;
    }

    updateDisplayedPixmap(imageAreaForDialogSize(event->size()));
}

void FilePreview::showContextMenu(const QPoint& pos)
{
    if (!saveCallback || !ui || !ui->fileContents) {
        return;
    }

    QMenu menu(this);
    menu.addAction(tr("Save As…"), this, [this] {
        const QString downloadDir =
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const QString suggestedPath =
            QDir(downloadDir).filePath(fileName);
        const QString destination = QFileDialog::getSaveFileName(
            this,
            tr("Save image as…"),
            suggestedPath);
        if (!destination.isEmpty() && saveCallback) {
            saveCallback(destination);
        }
    });
    menu.exec(ui->fileContents->mapToGlobal(pos));
}

} /* namespace Mattermost */
