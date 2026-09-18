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

#include "FilePreview.h"
#include "ui_FilePreview.h"

#include <algorithm>
#include <QFrame>
#include <QScrollArea>
#include <QVBoxLayout>
#if QT_VERSION < QT_VERSION_CHECK(6,0,0)
#include <QDesktopWidget>
#endif

namespace Mattermost  {

FilePreview::FilePreview(const FilePreviewData& file, QWidget* parent)
    : FilePreview(QImage::fromData(file.fileContents),
                  file.fileName,
                  file.fileAuthor,
                  parent)
{
}

FilePreview::FilePreview(const QImage& image,
                         const QString& fileName,
                         const QString& fileAuthor,
                         QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::FilePreview)
{
    ui->setupUi(this);
    setWindowTitle(fileName + " [" + fileAuthor + "] - Mattermost");

    pixmap = QPixmap::fromImage(image);
    ui->fileContents->setPixmap(pixmap);
    ui->fileContents->setScaledContents(true);
    ui->fileContents->setAlignment(Qt::AlignCenter);

    // Keep the dialog itself bounded. Very wide screenshots remain readable by
    // scrolling horizontally instead of collapsing their short side to a few
    // dozen pixels just to preserve the whole aspect ratio on screen.
    ui->verticalLayout->removeWidget(ui->fileContents);
    scrollArea = new QScrollArea(ui->frame);
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setWidgetResizable(false);
    scrollArea->setAlignment(Qt::AlignCenter);
    scrollArea->setWidget(ui->fileContents);
    ui->verticalLayout->addWidget(scrollArea);

    ui->fileInfo->setText(fileName);

    const QSize viewport = initialViewportSize();
    scrollArea->setMinimumSize(viewport);
    updateImageGeometry(viewport);
    adjustSize();
}

FilePreview::~FilePreview()
{
    delete ui;
}

QSize FilePreview::displaySizeForViewport(const QSize& viewportSize) const
{
    if (pixmap.isNull() || viewportSize.isEmpty()) {
        return QSize(1, 1);
    }

    QSize fit = pixmap.size();
    fit.scale(viewportSize, Qt::KeepAspectRatio);

    // Do not upscale ordinary small images.
    if (fit.width() > pixmap.width() || fit.height() > pixmap.height()) {
        fit = pixmap.size();
    }

    constexpr qreal ExtremeAspectRatio = 4.0;
    constexpr int MinReadableShortSide = 160;

    const QSize source = pixmap.size();
    const bool veryWide =
        source.height() > 0
        && static_cast<qreal>(source.width()) / source.height()
               >= ExtremeAspectRatio;
    const bool veryTall =
        source.width() > 0
        && static_cast<qreal>(source.height()) / source.width()
               >= ExtremeAspectRatio;

    if ((veryWide && fit.height() < MinReadableShortSide)
        || (veryTall && fit.width() < MinReadableShortSide)) {
        const int sourceShortSide =
            veryWide ? source.height() : source.width();
        const qreal readableScale = std::min<qreal>(
            1.0,
            static_cast<qreal>(MinReadableShortSide) / sourceShortSide);
        const QSize readable(
            std::max(1, qRound(source.width() * readableScale)),
            std::max(1, qRound(source.height() * readableScale)));

        if ((veryWide && readable.height() > fit.height())
            || (veryTall && readable.width() > fit.width())) {
            return readable;
        }
    }

    return fit.expandedTo(QSize(1, 1));
}

QSize FilePreview::initialViewportSize() const
{
#if QT_VERSION < QT_VERSION_CHECK(6,0,0)
    QRect screenGeometry = QApplication::desktop()->screenGeometry(this);
#else
    QRect screenGeometry = QGuiApplication::primaryScreen()->geometry();
#endif
    const QSize bounds(
        std::max(240, qRound(screenGeometry.width() * 0.9)),
        std::max(180, qRound(screenGeometry.height() * 0.8)));

    const QSize display = displaySizeForViewport(bounds);
    return QSize(
        std::min(display.width(), bounds.width()),
        std::min(display.height(), bounds.height()));
}

void FilePreview::updateImageGeometry(const QSize& viewportSize)
{
    if (!ui || !ui->fileContents || pixmap.isNull()) {
        return;
    }

    const QSize display = displaySizeForViewport(viewportSize);
    ui->fileContents->setFixedSize(display);
}

} /* namespace Mattermost */