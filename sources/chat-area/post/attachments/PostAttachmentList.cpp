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

#include "PostAttachmentList.h"
#include "ui_PostAttachmentList.h"

#include <QDebug>
#include <QEvent>
#include <QLabel>
#include <QListWidgetItem>
#include <QTimer>
#include "AttachedBinaryFile.h"
#include "AttachedImageFile.h"
#include "AttachedVideoFile.h"
#include "backend/types/BackendFile.h"

namespace Mattermost {

PostAttachmentList::PostAttachmentList (Backend& backend, QWidget *parent)
:QWidget(parent)
,backend (backend)
,ui (new Ui::PostAttachmentList)
{
    ui->setupUi(this);
    ui->verticalLayout->setContentsMargins(0, 0, 0, 0);
    ui->listWidget->viewport()->setAutoFillBackground(false);
    ui->listWidget->setSpacing(10);
    ui->listWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->listWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

PostAttachmentList::~PostAttachmentList()
{
    delete ui;
}

void PostAttachmentList::addFile (const BackendFile& file, const QString& authorName)
{
    auto* newItem = new QListWidgetItem();
    QWidget* fileWidget = nullptr;

#if BUILD_MULTIMEDIA
    if (file.name.endsWith(".mp4", Qt::CaseInsensitive) || file.name.endsWith(".mov", Qt::CaseInsensitive)) {
        fileWidget = new AttachedVideoFile (backend, file, this);
    } else
#endif
    if (!file.mimeType.startsWith("image")) {
        fileWidget = new AttachedBinaryFile (backend, file, this);
    } else {
        auto* imageWidget = new AttachedImageFile (backend, file, authorName, this);
        fileWidget = imageWidget;
        connect(imageWidget, &AttachedImageFile::dimensionsChanged, this,
                [this, newItem, fileWidget] {
            newItem->setSizeHint(fileWidget->size());
            updateDimensions();
        });
    }

    ui->listWidget->addItem(newItem);
    ui->listWidget->setItemWidget(newItem, fileWidget);

    fileWidget->adjustSize();
    newItem->setSizeHint(fileWidget->sizeHint().expandedTo(fileWidget->minimumSizeHint()));
    updateDimensions();
}

void PostAttachmentList::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (!event || event->type() != QEvent::FontChange) {
        return;
    }

    // Font propagation reaches the item widgets after their parent chain.
    // Recompute on the next event turn so every child has its new metrics.
    QTimer::singleShot(0, this, [this] {
        refreshItemSizeHints();
        updateDimensions();
    });
}

void PostAttachmentList::refreshItemSizeHints()
{
    for (int i = 0; i < ui->listWidget->count(); ++i) {
        QListWidgetItem* item = ui->listWidget->item(i);
        QWidget* widget = item ? ui->listWidget->itemWidget(item) : nullptr;
        if (!item || !widget) {
            continue;
        }

        widget->adjustSize();
        const QSize size = widget->sizeHint().expandedTo(widget->minimumSizeHint());
        widget->resize(size);
        item->setSizeHint(size);
    }
}

void PostAttachmentList::updateDimensions()
{
    const QSize listSize = ui->listWidget->sizeHint().expandedTo(QSize(1, 1));
    ui->listWidget->setFixedSize(listSize);

    if (layout()) {
        layout()->activate();
        setFixedSize(layout()->sizeHint());
    } else {
        setFixedSize(listSize);
    }

    updateGeometry();
    emit dimensionsChanged();
}

} /* namespace Mattermost */
