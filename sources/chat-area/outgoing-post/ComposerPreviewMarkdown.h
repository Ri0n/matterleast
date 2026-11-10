#pragma once

#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include "LocalAttachmentMarkdown.h"
#include "OutgoingAttachmentList.h"

namespace Mattermost::ComposerPreviewMarkdown {

struct Prepared {
    QString markdown;
    QHash<QString, QString> imagePaths;
};

inline bool isImageFile(const QString& path)
{
    const QString extension = QFileInfo(path).suffix().toLower();
    return extension == QLatin1String("png")
        || extension == QLatin1String("jpg")
        || extension == QLatin1String("jpeg")
        || extension == QLatin1String("gif")
        || extension == QLatin1String("webp")
        || extension == QLatin1String("bmp")
        || extension == QLatin1String("svg");
}

inline Prepared prepare(const QString& markdown,
                        const QList<OutgoingAttachmentItem>& attachments)
{
    Prepared preview;
    preview.markdown = markdown;

    QHash<QString, QString> paths;
    for (const OutgoingAttachmentItem& attachment : attachments) {
        if (!attachment.id.isEmpty() && isImageFile(attachment.path)) {
            paths.insert(attachment.id, attachment.path);
        }
    }

    // Reuse the received-message renderer: the temporary URL has the same
    // shape as a delivered inline image but is resolved only to local bytes.
    QSet<QString> referencedImages;
    const auto references = LocalAttachmentMarkdown::imageReferences(markdown);
    for (auto it = references.crbegin(); it != references.crend(); ++it) {
        if (!paths.contains(it->attachmentId)) {
            continue;
        }
        preview.markdown.replace(
            it->uriStart, it->uriLength,
            QStringLiteral("/api/v4/files/") + it->attachmentId);
        referencedImages.insert(it->attachmentId);
    }

    // Clipboard-pasted images are attached as local PNGs, not inserted into
    // the Markdown editor. Show them after the text, just as the sent post
    // shows an attachment that was not explicitly placed inline.
    for (const OutgoingAttachmentItem& attachment : attachments) {
        if (!paths.contains(attachment.id)
            || referencedImages.contains(attachment.id)) {
            continue;
        }
        QString label = QFileInfo(attachment.path).fileName();
        label.replace(QLatin1Char('\n'), QLatin1Char(' '));
        if (!preview.markdown.isEmpty()) {
            preview.markdown += QStringLiteral("\n\n");
        }
        preview.markdown += QStringLiteral("![%1](/api/v4/files/%2)")
            .arg(LocalAttachmentMarkdown::escapeLabel(label), attachment.id);
    }

    preview.imagePaths = std::move(paths);
    return preview;
}

} // namespace Mattermost::ComposerPreviewMarkdown
