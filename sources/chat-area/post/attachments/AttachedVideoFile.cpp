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

#include "build-config.h"

#if BUILD_MULTIMEDIA

#include "AttachedVideoFile.h"

#include <QBuffer>
#include <QPointer>
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QMediaContent>
#endif

#include "ui_AttachedVideoFile.h"
#include "backend/AttachmentService.h"
#include "backend/types/BackendFile.h"

namespace Mattermost {

AttachedVideoFile::AttachedVideoFile(Backend& backend, const BackendFile& file, QWidget* parent)
    : QWidget(parent)
    , ui(new Ui::AttachedVideoFile)
    , backend(backend)
    , mediaPlayer(nullptr)
    , videoWidget(nullptr)
    , fileId(file.id)
    , init(true)
{
    ui->setupUi(this);
    videoWidget = new QVideoWidget(parent);
    mediaPlayer = new QMediaPlayer(this);

    ui->videoName->setText(file.name);
    ui->verticalLayout->addWidget(videoWidget);
    mediaPlayer->setVideoOutput(videoWidget);
    videoWidget->show();
}

AttachedVideoFile::~AttachedVideoFile()
{
    delete ui;
}

void AttachedVideoFile::mousePressEvent(QMouseEvent*)
{
    QPointer<AttachedVideoFile> self(this);
    AttachmentService::instance(backend).retrieveFile(fileId, [self](const QByteArray& data) {
        if (!self) {
            return;
        }

        QBuffer* previousStream = self->mediaStream;
        self->mediaStream = new QBuffer(self->mediaPlayer);
        self->mediaStream->setData(data);
        if (!self->mediaStream->open(QIODevice::ReadOnly)) {
            self->mediaStream->deleteLater();
            self->mediaStream = previousStream;
            return;
        }

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        self->mediaPlayer->setSourceDevice(self->mediaStream);
#else
        self->mediaPlayer->setMedia(QMediaContent(), self->mediaStream);
#endif
        if (previousStream) {
            previousStream->deleteLater();
        }
        self->mediaPlayer->play();
    });
}

} /* namespace Mattermost */

#endif // BUILD_MULTIMEDIA
