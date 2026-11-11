#include "UserMentionLinkifier.h"

#include <QPointer>
#include <QSet>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QVector>

#include "backend/ChannelReferenceService.h"

namespace Mattermost::UserMentionLinkifier {
namespace {

// Inspect only links created by linkify(). Do not reinterpret explicit
// Markdown links, code or text using a second channel-name parser.
QString pendingSlug(const QTextFragment& fragment, const QString& prefix)
{
    if (!fragment.isValid() || !fragment.charFormat().isAnchor()) {
        return {};
    }
    const QString href = fragment.charFormat().anchorHref();
    if (!href.startsWith(prefix)) {
        return {};
    }
    const QString slug = href.mid(prefix.size());
    return !slug.isEmpty() && fragment.text() == QLatin1Char('~') + slug
        ? slug : QString();
}

} // namespace

void resolveChannelDisplayNames(QTextDocument& document, Backend& backend,
                                const QString& teamName, QObject* owner,
                                std::function<void()> changed)
{
    if (!owner || teamName.isEmpty()) {
        return;
    }

    const QString prefix = QStringLiteral("/") + teamName
        + QStringLiteral("/channels/");
    QSet<QString> slugs;
    for (QTextBlock block = document.begin(); block.isValid();
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QString slug = pendingSlug(it.fragment(), prefix);
            if (!slug.isEmpty()) {
                slugs.insert(slug);
            }
        }
    }

    QPointer<QTextDocument> doc(&document);
    QPointer<QObject> guard(owner);
    for (const QString& slug : slugs) {
        ChannelReferenceService::instance(backend).resolve(
            teamName, slug,
            [doc, guard, prefix, slug, changed](const QString& name) {
                if (!guard || !doc || name.isEmpty() || name == slug) {
                    return;
                }

                struct Range {
                    int start;
                    int length;
                    QTextCharFormat format;
                };
                QVector<Range> ranges;
                for (QTextBlock block = doc->begin(); block.isValid();
                     block = block.next()) {
                    for (auto it = block.begin(); !it.atEnd(); ++it) {
                        const QTextFragment fragment = it.fragment();
                        if (pendingSlug(fragment, prefix) == slug) {
                            ranges.push_back({fragment.position(),
                                              fragment.length(),
                                              fragment.charFormat()});
                        }
                    }
                }

                // Replacing from the end leaves earlier document offsets intact.
                for (auto it = ranges.crbegin(); it != ranges.crend(); ++it) {
                    QTextCursor cursor(doc);
                    cursor.setPosition(it->start);
                    cursor.setPosition(it->start + it->length,
                                       QTextCursor::KeepAnchor);
                    cursor.insertText(QLatin1Char('~') + name, it->format);
                }
                if (!ranges.isEmpty() && changed) {
                    changed();
                }
            });
    }
}

} // namespace Mattermost::UserMentionLinkifier
