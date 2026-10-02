#pragma once

#include <QList>
#include <QRegularExpression>
#include <QString>

namespace Mattermost::LocalAttachmentMarkdown {

inline constexpr char SchemePrefix[] = "matterleast-attachment:";

struct ImageReference {
    int markdownStart = -1;
    int markdownLength = 0;
    int uriStart = -1;
    int uriLength = 0;
    QString attachmentId;
    int attachmentIndex = -1;
};

inline QString escapeLabel(QString label)
{
    label.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    label.replace(QLatin1Char('['), QStringLiteral("\\["));
    label.replace(QLatin1Char(']'), QStringLiteral("\\]"));
    return label;
}

inline QString uri(const QString& attachmentId, int attachmentIndex)
{
    return QStringLiteral("matterleast-attachment:%1@%2")
        .arg(attachmentId)
        .arg(attachmentIndex);
}

inline QString image(const QString& label,
                     const QString& attachmentId,
                     int attachmentIndex)
{
    return QStringLiteral("![%1](%2)")
        .arg(escapeLabel(label), uri(attachmentId, attachmentIndex));
}

inline const QRegularExpression& imageReferenceExpression()
{
    // Attachment IDs are QUuid::WithoutBraces strings. Keeping the grammar
    // deliberately narrow makes the internal transport marker impossible to
    // confuse with an ordinary user-authored URL.
    static const QRegularExpression expression(
        QStringLiteral(
            R"(!\[((?:\\.|[^\]\\\n])*)\]\((matterleast-attachment:([0-9A-Fa-f-]+)@([0-9]+))\))"));
    return expression;
}

inline QList<ImageReference> imageReferences(const QString& markdown)
{
    QList<ImageReference> references;
    auto matches = imageReferenceExpression().globalMatch(markdown);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        bool ok = false;
        const int index = match.captured(4).toInt(&ok);
        if (!ok) {
            continue;
        }
        references.push_back({
            static_cast<int>(match.capturedStart(0)),
            static_cast<int>(match.capturedLength(0)),
            static_cast<int>(match.capturedStart(2)),
            static_cast<int>(match.capturedLength(2)),
            match.captured(3),
            index,
        });
    }
    return references;
}

inline bool resolveForDelivery(const QString& markdown,
                               const QList<QString>& attachmentFileIds,
                               QString& resolvedMarkdown,
                               QString& errorText)
{
    resolvedMarkdown = markdown;
    errorText.clear();

    const int attachmentCount = static_cast<int>(attachmentFileIds.size());
    const QList<ImageReference> references = imageReferences(markdown);
    for (auto it = references.crbegin(); it != references.crend(); ++it) {
        const ImageReference& reference = *it;
        if (reference.attachmentIndex < 0
            || reference.attachmentIndex >= attachmentCount
            || attachmentFileIds.at(reference.attachmentIndex).isEmpty()) {
            errorText = QStringLiteral(
                "Inline image attachment %1 is not available for delivery")
                            .arg(reference.attachmentId);
            return false;
        }

        const QString remoteUrl = QStringLiteral("/api/v4/files/%1")
                                      .arg(attachmentFileIds.at(
                                          reference.attachmentIndex));
        resolvedMarkdown.replace(
            reference.uriStart, reference.uriLength, remoteUrl);
    }

    // Never leak an internal reference into Mattermost. This also catches a
    // malformed/user-edited marker that no longer matches the image grammar.
    if (resolvedMarkdown.contains(QString::fromLatin1(SchemePrefix))) {
        errorText = QStringLiteral(
            "Message contains an unresolved local attachment reference");
        return false;
    }
    return true;
}

} // namespace Mattermost::LocalAttachmentMarkdown
