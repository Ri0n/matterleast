/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#include "OutgoingPostCreator.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QTextCursor>

#include "LocalAttachmentMarkdown.h"
#include "OutgoingAttachmentList.h"

namespace Mattermost {
namespace {

constexpr char InlineImageTrackingProperty[] =
    "_matterleast_inline_image_attachment_tracking";

} // namespace

void OutgoingPostCreator::insertImages()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        tr("Insert image"),
        QString(),
        tr("Images (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg);;All files (*)"));
    if (files.isEmpty()) {
        return;
    }

    const int previousCount = attachmentList
        ? attachmentList->attachments().size() : 0;
    QStringList attachmentFiles = files;
    createAttachmentList(attachmentFiles);
    if (!attachmentList) {
        return;
    }

    if (!attachmentList->property(InlineImageTrackingProperty).toBool()) {
        attachmentList->setProperty(InlineImageTrackingProperty, true);
        connect(attachmentList,
                &OutgoingAttachmentList::fileRemoved,
                this,
                [this](const QString& itemId, const QString&) {
                    syncInlineImageAttachmentReferences(itemId);
                });
        connect(attachmentList,
                &OutgoingAttachmentList::fileAdded,
                this,
                [this](const QString&, const QString&) {
                    syncInlineImageAttachmentReferences();
                });
    }

    const QList<OutgoingAttachmentItem> items = attachmentList->attachments();
    if (previousCount >= items.size()) {
        return;
    }

    QStringList snippets;
    for (int index = previousCount; index < items.size(); ++index) {
        const OutgoingAttachmentItem& item = items.at(index);
        snippets.push_back(LocalAttachmentMarkdown::image(
            QFileInfo(item.path).fileName(), item.id, index));
    }

    QTextCursor cursor = textCursor();
    cursor.beginEditBlock();
    cursor.insertText(snippets.join(QLatin1Char('\n')));
    cursor.endEditBlock();
    setTextCursor(cursor);
    setFocus();
}

void OutgoingPostCreator::syncInlineImageAttachmentReferences(
    const QString& removedAttachmentId)
{
    const QString markdown = toPlainText();
    const QList<LocalAttachmentMarkdown::ImageReference> references =
        LocalAttachmentMarkdown::imageReferences(markdown);
    if (references.isEmpty()) {
        return;
    }

    QHash<QString, int> indexes;
    if (attachmentList) {
        const QList<OutgoingAttachmentItem> items = attachmentList->attachments();
        for (int index = 0; index < items.size(); ++index) {
            indexes.insert(items.at(index).id, index);
        }
    }

    QTextCursor editCursor(document());
    editCursor.beginEditBlock();
    for (auto it = references.crbegin(); it != references.crend(); ++it) {
        const LocalAttachmentMarkdown::ImageReference& reference = *it;
        if (!removedAttachmentId.isEmpty()
            && reference.attachmentId == removedAttachmentId) {
            editCursor.setPosition(reference.markdownStart);
            editCursor.setPosition(
                reference.markdownStart + reference.markdownLength,
                QTextCursor::KeepAnchor);
            editCursor.removeSelectedText();
            continue;
        }

        const auto indexIt = indexes.constFind(reference.attachmentId);
        if (indexIt == indexes.constEnd()
            || indexIt.value() == reference.attachmentIndex) {
            continue;
        }

        editCursor.setPosition(reference.uriStart);
        editCursor.setPosition(
            reference.uriStart + reference.uriLength,
            QTextCursor::KeepAnchor);
        editCursor.insertText(LocalAttachmentMarkdown::uri(
            reference.attachmentId, indexIt.value()));
    }
    editCursor.endEditBlock();
}

} // namespace Mattermost
