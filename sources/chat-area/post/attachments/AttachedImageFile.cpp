/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

#include "AttachedImageFile.h"
#include "ui_AttachedImageFile.h"

#include <algorithm>
#include <utility>

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QLayout>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QStandardPaths>
#include "backend/types/BackendFile.h"
#include "backend/AttachmentService.h"
#include "Settings.h"
#include "options/MLOptions.h"

namespace {

QPixmap roundedPixmap(const QPixmap& source, qreal radius)
{
    if (source.isNull() || radius <= 0) {
        return source;
    }

    QPixmap rounded(source.size());
    rounded.fill(Qt::transparent);

    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing, true);

    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, source.width(), source.height()), radius, radius);
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, source);

    return rounded;
}

} // namespace

namespace Mattermost {

std::map <const QWidget*, FilePreview*> AttachedImageFile::currentlyOpenFiles;

AttachedImageFile::AttachedImageFile (Backend& backend, const BackendFile& file, const QString&, QWidget *parent)
:QWidget(parent)
,ui(new Ui::AttachedImageFile)
,fileId(file.id)
,backend(backend)
{
    ui->setupUi(this);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    setToolTip(file.name);
    ui->imagePreview->setToolTip(file.name);
    ui->imagePreview->clear();
    ui->imagePreview->hide();

    const auto refreshPreview = [this](const QVariant&) {
        updatePreviewPixmap();
    };
    auto* maxWidth = MLOptions::instance()->optionObject<int>(
        DOWNLOAD_IMAGE_MAX_WIDTH, DOWNLOAD_IMAGE_MAX_WIDTH_DEFAULT);
    auto* maxHeight = MLOptions::instance()->optionObject<int>(
        DOWNLOAD_IMAGE_MAX_HEIGHT, DOWNLOAD_IMAGE_MAX_HEIGHT_DEFAULT);
    connect(maxWidth, &MLOptionObject::changed, this, refreshPreview);
    connect(maxHeight, &MLOptionObject::changed, this, refreshPreview);

    // Do not let the designer-time 400x300 geometry participate in the list
    // item's initial size while the image is still being downloaded.
    setFixedSize(1, 1);

    const QString fileName = file.name;
    QPointer<AttachedImageFile> self(this);

    AttachmentService::instance(backend).retrieveFile(fileId, [self](const QByteArray& fileContents) {
        if (!self) {
            return;
        }

        QPixmap pixmap;
        if (!pixmap.loadFromData(fileContents)) {
            return;
        }
        self->setPreviewPixmap(std::move(pixmap));
    });

    connect(this, &QWidget::customContextMenuRequested, this,
            [this, fileName](const QPoint& pos) {
        QMenu menu(this);

        menu.addAction("Save image", this, [this, fileName] {
            const QString defaultDownloadDir =
                QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            const QDir downloadDir(
                MLOptions::instance()
                    ->optionObject<QString>(DOWNLOAD_LOCATION, defaultDownloadDir)
                    ->value().toString());
            const QString saveFileDestination = QFileDialog::getSaveFileName(
                this, "Save image as... - Mattermost", downloadDir.filePath(fileName));

            if (saveFileDestination.isEmpty()) {
                return;
            }

            AttachmentService::instance(this->backend).retrieveFile(fileId, [saveFileDestination](const QByteArray& fileContents) {
                QFile destFile(saveFileDestination);
                if (!destFile.open(QIODevice::WriteOnly)) {
                    qWarning() << "Cannot save image to" << saveFileDestination << ":" << destFile.errorString();
                    return;
                }
                destFile.write(fileContents);
                destFile.close();
            });
        });

        menu.exec(mapToGlobal(pos) + QPoint(10, 0));
    });
}

AttachedImageFile::~AttachedImageFile()
{
    currentlyOpenFiles.erase(this);
    delete ui;
}

void AttachedImageFile::setPreviewPixmap(QPixmap pixmap)
{
    sourcePixmap = std::move(pixmap);
    updatePreviewPixmap();
}

void AttachedImageFile::updatePreviewPixmap()
{
    if (sourcePixmap.isNull()) {
        return;
    }

    const int maxWidth = std::max(
        1,
        MLOptions::instance()
            ->optionObject<int>(
                DOWNLOAD_IMAGE_MAX_WIDTH, DOWNLOAD_IMAGE_MAX_WIDTH_DEFAULT)
            ->value().toInt());
    const int maxHeight = std::max(
        1,
        MLOptions::instance()
            ->optionObject<int>(
                DOWNLOAD_IMAGE_MAX_HEIGHT, DOWNLOAD_IMAGE_MAX_HEIGHT_DEFAULT)
            ->value().toInt());

    QPixmap pixmap = sourcePixmap;
    if (pixmap.width() > maxWidth || pixmap.height() > maxHeight) {
        pixmap = pixmap.scaled(
            QSize(maxWidth, maxHeight), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    pixmap = roundedPixmap(pixmap, 5.0);

    ui->imagePreview->setPixmap(pixmap);
    ui->imagePreview->setFixedSize(pixmap.size());
    ui->imagePreview->show();

    if (layout()) {
        layout()->activate();
        setFixedSize(layout()->sizeHint().expandedTo(QSize(1, 1)));
    } else {
        setFixedSize(pixmap.size().expandedTo(QSize(1, 1)));
    }

    updateGeometry();
    emit dimensionsChanged();
}

void AttachedImageFile::mouseReleaseEvent(QMouseEvent*)
{
    QPointer<AttachedImageFile> self(this);
    AttachmentService::instance(backend).retrieveFile(fileId, [self](const QByteArray& fileContents) {
        if (!self) {
            return;
        }

        const QWidget* const key = self.data();
        auto openFile = currentlyOpenFiles.find(key);
        FilePreview* filePreview = nullptr;

        if (openFile == currentlyOpenFiles.end()) {
            FilePreviewData previewData { fileContents, "", "" };
            filePreview = new FilePreview(previewData, nullptr);
            currentlyOpenFiles.emplace(key, filePreview);
            filePreview->setAttribute(Qt::WA_DeleteOnClose);
            filePreview->show();

            connect(filePreview, &QDialog::rejected, filePreview, [key, filePreview] {
                qDebug() << "Rejected";
                auto it = AttachedImageFile::currentlyOpenFiles.find(key);
                if (it != AttachedImageFile::currentlyOpenFiles.end() && it->second == filePreview) {
                    AttachedImageFile::currentlyOpenFiles.erase(it);
                }
            });
        } else {
            filePreview = openFile->second;
            filePreview->raise();
            filePreview->activateWindow();
        }
    });
}

} /* namespace Mattermost */
