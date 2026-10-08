/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of MatterLeast.
 *
 * MatterLeast is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MatterLeast is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with MatterLeast. if not, see https://www.gnu.org/licenses/.
 */

#include "PostAttachmentList.h"
#include "ui_PostAttachmentList.h"

#include <QDebug>
#include <QEvent>
#include <QLabel>
#include <QListWidgetItem>
#include <QLayout>
#include <QSizePolicy>
#include <QTimer>
#include "AttachedBinaryFile.h"
#include "AttachedImageFile.h"
#include "AttachedVideoFile.h"
#include "backend/types/BackendFile.h"
#include "chat-area/post/PostWidget.h"

namespace Mattermost {

PostAttachmentList::PostAttachmentList (Backend& backend, QWidget *parent)
:QWidget(parent)
,backend (backend)
,ui (new Ui::PostAttachmentList)
{
    ui->setupUi(this);
    ui->verticalLayout->setContentsMargins(0, 0, 0, 0);
    ui->listWidget->viewport()->setAutoFillBackground(false);
    ui->listWidget->viewport()->installEventFilter(this);
    // QListView spacing also adds a gutter before the first and after the last
    // item. Keep it compact; the previous 10px produced an empty bottom strip.
    ui->listWidget->setSpacing(4);
    ui->listWidget->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    ui->listWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // PostWidget creates the list before it enumerates its files. Keep the
    // shell out of layout geometry until addFile() finds at least one file that
    // was not claimed by a Markdown image in the message body.
    setVisible(false);

    if (auto* postWidget = qobject_cast<PostWidget*>(parent)) {
        postWidget->prepareInlineAttachmentContext();
    }
}

PostAttachmentList::~PostAttachmentList()
{
    delete ui;
}

void PostAttachmentList::addFile (const BackendFile& file, const QString& authorName)
{
    if (auto* postWidget = qobject_cast<PostWidget*>(parentWidget())) {
        if (postWidget->isAttachmentRenderedInline(file.id)) {
            return;
        }
    }

    auto* newItem = new QListWidgetItem();
    QWidget* fileWidget = nullptr;

#if BUILD_MULTIMEDIA
    if (file.name.endsWith(".mp4", Qt::CaseInsensitive) || file.name.endsWith(".mov", Qt::CaseInsensitive)) {
        fileWidget = new AttachedVideoFile (backend, file, this);
    } else
#endif
    if (!file.mimeType.startsWith("image")) {
        auto* binaryWidget = new AttachedBinaryFile(backend, file, this);
        fileWidget = binaryWidget;
        connect(binaryWidget, &AttachedBinaryFile::dimensionsChanged, this,
                [this] {
            refreshItemSizeHints();
            updateDimensions();
        });
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
    setVisible(true);

    refreshItemSizeHints();
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

bool PostAttachmentList::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == ui->listWidget->viewport() && event
        && event->type() == QEvent::Resize) {
        const int width = ui->listWidget->viewport()->width();
        if (width != _lastMeasuredViewportWidth) {
            _lastMeasuredViewportWidth = width;
            // Let the QListWidget viewport finish its resize before computing
            // the height-for-width of wrapping attachment text.
            QTimer::singleShot(0, this, [this] {
                refreshItemSizeHints();
                updateDimensions();
            });
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PostAttachmentList::refreshItemSizeHints()
{
    const int availableWidth = std::max(1, ui->listWidget->viewport()->width()
                                             - 2 * ui->listWidget->spacing());
    for (int i = 0; i < ui->listWidget->count(); ++i) {
        QListWidgetItem* item = ui->listWidget->item(i);
        QWidget* widget = item ? ui->listWidget->itemWidget(item) : nullptr;
        if (!item || !widget) {
            continue;
        }

        QSize size;
        if (qobject_cast<AttachedBinaryFile*>(widget)) {
            // QListWidgetItem caches a size hint, while wrapping QLabel has
            // height-for-width. A height calculated before the post grows
            // would otherwise keep phantom lines and a blank bottom strip.
            int height = -1;
            if (QLayout* content = widget->layout()) {
                if (content->hasHeightForWidth()) {
                    height = content->heightForWidth(availableWidth);
                }
            }
            if (height < 0) {
                height = widget->sizeHint().height();
            }
            size = QSize(availableWidth, std::max(height, widget->minimumHeight()));
        } else {
            widget->adjustSize();
            size = widget->sizeHint().expandedTo(widget->minimumSizeHint());
        }
        if (item->sizeHint() != size) {
            item->setSizeHint(size);
        }
    }
}

void PostAttachmentList::updateDimensions()
{
    const QSize listSize = ui->listWidget->sizeHint().expandedTo(QSize(1, 1));
    // The surrounding post layout owns horizontal geometry. Only the
    // attachments' content height is intrinsic; fixed width truncated paths.
    ui->listWidget->setFixedHeight(listSize.height());
    ui->listWidget->setMinimumWidth(0);
    setMinimumWidth(0);
    updateGeometry();
    emit dimensionsChanged();
}

} /* namespace Mattermost */
