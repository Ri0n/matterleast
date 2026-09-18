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
#include <functional>
#include <utility>

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QImage>
#include <QLayout>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QRunnable>
#include <QStandardPaths>
#include <QThreadPool>
#include "backend/types/BackendFile.h"
#include "backend/AttachmentService.h"
#include "Settings.h"
#include "options/MLOptions.h"

namespace {

class ImageDecodeTask final : public QRunnable
{
public:
    using Callback = std::function<void(QImage)>;

    ImageDecodeTask(QByteArray data, Callback callback)
        : data(std::move(data))
        , callback(std::move(callback))
    {
        setAutoDelete(true);
    }

    void run() override
    {
        QImage image = QImage::fromData(data);
        QObject* dispatcher = QCoreApplication::instance();
        if (!dispatcher) {
            return;
        }

        QMetaObject::invokeMethod(
            dispatcher,
            [callback = std::move(callback), image = std::move(image)]() mutable {
                if (callback) {
                    callback(std::move(image));
                }
            },
            Qt::QueuedConnection);
    }

private:
    QByteArray data;
    Callback callback;
};

void decodeImageAsync(const QByteArray& data, ImageDecodeTask::Callback callback)
{
    QThreadPool::globalInstance()->start(
        new ImageDecodeTask(data, std::move(callback)));
}

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

AttachedImageFile::AttachedImageFile(Backend& backend,
                                     const BackendFile& file,
                                     const QString& authorName,
                                     QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::AttachedImageFile)
    , fileId(file.id)
    , fileName(file.name)
    , fileAuthor(authorName)
    , fileMimeType(file.mimeType)
    , fileExtension(file.extension)
    , backend(backend)
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

    const QString attachmentFileName = file.name;
    QPointer<AttachedImageFile> self(this);

    const bool isSvg =
        file.mimeType.compare(QStringLiteral("image/svg+xml"), Qt::CaseInsensitive) == 0
        || file.extension.compare(QStringLiteral("svg"), Qt::CaseInsensitive) == 0;

    const auto requestThumbnailFallback = [self] {
        if (!self) {
            return;
        }

        AttachmentService::instance(self->backend).retrieveThumbnail(
            self->fileId,
            [self](const QByteArray& thumbnailContents) {
                if (!self) {
                    return;
                }
                if (thumbnailContents.isEmpty()) {
                    self->showPreviewFallback();
                    return;
                }

                decodeImageAsync(
                    thumbnailContents,
                    [self](QImage image) {
                        if (!self) {
                            return;
                        }
                        if (image.isNull()) {
                            self->showPreviewFallback();
                            return;
                        }
                        self->setPreviewPixmap(QPixmap::fromImage(image));
                    });
            });
    };

    // Mattermost thumbnails are only 120x100. They are useful as a fallback,
    // but using them as the primary inline source turns panoramic screenshots
    // into an unreadable strip. Use the server-generated preview (up to 1920px
    // wide) and fit that into the configured preview rectangle instead.
    //
    // SVG files deliberately stay out of the local decode path: pathological
    // SVG filter graphs were the source of the old multi-second GUI stalls.
    if (isSvg) {
        showPreviewFallback();
    } else {
        AttachmentService::instance(backend).retrievePreview(
            fileId,
            [self, requestThumbnailFallback](const QByteArray& previewContents) {
                if (!self) {
                    return;
                }
                if (previewContents.isEmpty()) {
                    requestThumbnailFallback();
                    return;
                }

                decodeImageAsync(
                    previewContents,
                    [self, requestThumbnailFallback](QImage image) {
                        if (!self) {
                            return;
                        }
                        if (image.isNull()) {
                            requestThumbnailFallback();
                            return;
                        }
                        self->setPreviewPixmap(QPixmap::fromImage(image));
                    });
            });
    }

    connect(this, &QWidget::customContextMenuRequested, this,
            [this, attachmentFileName](const QPoint& pos) {
        QMenu menu(this);

        menu.addAction("Save image", this, [this, attachmentFileName] {
            const QString defaultDownloadDir =
                QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            const QDir downloadDir(
                MLOptions::instance()
                    ->optionObject<QString>(DOWNLOAD_LOCATION, defaultDownloadDir)
                    ->value().toString());
            const QString saveFileDestination = QFileDialog::getSaveFileName(
                this, "Save image as... - Mattermost", downloadDir.filePath(attachmentFileName));

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
    ui->imagePreview->setText(QString());
    ui->imagePreview->setMargin(0);
    ui->imagePreview->setStyleSheet(QString());
    updatePreviewPixmap();
}

void AttachedImageFile::showPreviewFallback()
{
    const int maxWidth = std::max(
        120,
        MLOptions::instance()
            ->optionObject<int>(
                DOWNLOAD_IMAGE_MAX_WIDTH, DOWNLOAD_IMAGE_MAX_WIDTH_DEFAULT)
            ->value().toInt());

    const int availableTextWidth = std::max(80, std::min(maxWidth, 360) - 16);
    const QString displayName = fontMetrics().elidedText(
        fileName, Qt::ElideMiddle, availableTextWidth);

    ui->imagePreview->setPixmap(QPixmap());
    ui->imagePreview->setText(displayName);
    ui->imagePreview->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui->imagePreview->setMargin(7);
    ui->imagePreview->setStyleSheet(QStringLiteral(
        "QLabel { border: 1px solid palette(mid); border-radius: 4px; }"));
    ui->imagePreview->setFixedSize(
        std::min(maxWidth, availableTextWidth + 16),
        fontMetrics().height() + 16);
    ui->imagePreview->show();

    if (layout()) {
        layout()->activate();
        setFixedSize(layout()->sizeHint().expandedTo(QSize(1, 1)));
    } else {
        setFixedSize(ui->imagePreview->size());
    }

    updateGeometry();
    emit dimensionsChanged();
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
    const QWidget* const key = this;
    auto openFile = currentlyOpenFiles.find(key);
    if (openFile != currentlyOpenFiles.end()) {
        openFile->second->raise();
        openFile->second->activateWindow();
        return;
    }

    if (fullPreviewDecodePending) {
        return;
    }
    fullPreviewDecodePending = true;

    QPointer<AttachedImageFile> self(this);

    const auto showDecodedImage = [self](QImage image) {
        if (!self) {
            return;
        }
        self->fullPreviewDecodePending = false;
        if (image.isNull()) {
            return;
        }

        const QWidget* const key = self.data();
        auto openFile = currentlyOpenFiles.find(key);
        if (openFile != currentlyOpenFiles.end()) {
            openFile->second->raise();
            openFile->second->activateWindow();
            return;
        }

        auto* filePreview = new FilePreview(
            image, self->fileName, self->fileAuthor, nullptr);
        currentlyOpenFiles.emplace(key, filePreview);
        filePreview->setAttribute(Qt::WA_DeleteOnClose);
        filePreview->show();

        connect(
            filePreview,
            &QDialog::rejected,
            filePreview,
            [key, filePreview] {
                auto it = AttachedImageFile::currentlyOpenFiles.find(key);
                if (it != AttachedImageFile::currentlyOpenFiles.end()
                    && it->second == filePreview) {
                    AttachedImageFile::currentlyOpenFiles.erase(it);
                }
            });
    };

    const auto requestServerPreview = [self, showDecodedImage] {
        if (!self) {
            return;
        }
        AttachmentService::instance(self->backend).retrievePreview(
            self->fileId,
            [self, showDecodedImage](const QByteArray& previewContents) {
                if (!self) {
                    return;
                }
                if (previewContents.isEmpty()) {
                    self->fullPreviewDecodePending = false;
                    return;
                }
                decodeImageAsync(
                    previewContents,
                    [self, showDecodedImage](QImage image) {
                        if (!self) {
                            return;
                        }
                        if (image.isNull()) {
                            self->fullPreviewDecodePending = false;
                            return;
                        }
                        showDecodedImage(std::move(image));
                    });
            });
    };

    const bool isSvg =
        fileMimeType.compare(QStringLiteral("image/svg+xml"), Qt::CaseInsensitive) == 0
        || fileExtension.compare(QStringLiteral("svg"), Qt::CaseInsensitive) == 0;

    if (isSvg) {
        // Never decode the original SVG locally. Keep the old server-side
        // rasterization safety boundary for expensive filter graphs.
        requestServerPreview();
        return;
    }

    // Opening a raster image is an explicit user action, so use the original
    // bytes instead of Mattermost's 1920px server preview. Decode still happens
    // off the GUI thread. Fall back to /preview for older/unavailable files.
    AttachmentService::instance(backend).retrieveFile(
        fileId,
        [self, showDecodedImage, requestServerPreview](const QByteArray& contents) {
            if (!self) {
                return;
            }
            if (contents.isEmpty()) {
                requestServerPreview();
                return;
            }

            decodeImageAsync(
                contents,
                [self, showDecodedImage, requestServerPreview](QImage image) {
                    if (!self) {
                        return;
                    }
                    if (image.isNull()) {
                        requestServerPreview();
                        return;
                    }
                    showDecodedImage(std::move(image));
                });
        });
}

} /* namespace Mattermost */
