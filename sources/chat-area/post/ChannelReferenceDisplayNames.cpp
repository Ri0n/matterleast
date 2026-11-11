#include "ChannelReferenceDisplayNames.h"

#include <utility>

#include <QPointer>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QVector>

#include "backend/ChannelReferenceService.h"

namespace Mattermost {
namespace ChannelReferenceDisplayNames {
namespace {

QString routePrefix(const QString& teamName)
{
    return QStringLiteral("/") + teamName + QStringLiteral("/channels/");
}

// The linkifier has already identified real ~channel references. Inspect
// anchor fragments rather than duplicating its regex or matching code spans,
// existing Markdown hyperlinks and plain text a second time.
QString unresolvedSlug(const QTextFragment& fragment, const QString& teamName)
{
    if (!fragment.isValid() || !fragment.charFormat().isAnchor()
        || teamName.isEmpty()) {
        return {};
    }
    const QString href = fragment.charFormat().anchorHref();
    const QString prefix = routePrefix(teamName);
    if (!href.startsWith(prefix)) {
        return {};
    }
    const QString slug = href.mid(prefix.size());
    if (slug.isEmpty() || fragment.text() != QLatin1Char('~') + slug) {
        return {};
    }
    return slug;
}

} // namespace

QSet<QString> unresolvedSlugs(const QTextDocument& document,
                              const QString& teamName)
{
    QSet<QString> slugs;
    for (QTextBlock block = document.begin(); block.isValid();
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QString slug = unresolvedSlug(it.fragment(), teamName);
            if (!slug.isEmpty()) {
                slugs.insert(slug);
            }
        }
    }
    return slugs;
}

bool replace(QTextDocument& document, const QString& teamName,
             const QString& slug, const QString& displayName)
{
    if (teamName.isEmpty() || slug.isEmpty() || displayName.isEmpty()
        || slug == displayName) {
        return false;
    }

    struct Occurrence {
        int position;
        int length;
        QTextCharFormat format;
    };
    QVector<Occurrence> positions;
    for (QTextBlock block = document.begin(); block.isValid();
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (unresolvedSlug(fragment, teamName) == slug) {
                positions.push_back(
                    {fragment.position(), fragment.length(), fragment.charFormat()});
            }
        }
    }

    // Replace backwards because each human-readable display name can change
    // character count. Preserve the original href for internal navigation.
    for (auto it = positions.crbegin(); it != positions.crend(); ++it) {
        QTextCursor cursor(&document);
        cursor.setPosition(it->position);
        cursor.setPosition(it->position + it->length, QTextCursor::KeepAnchor);
        cursor.insertText(QLatin1Char('~') + displayName, it->format);
    }
    return !positions.isEmpty();
}

void resolve(QTextDocument& document, Backend& backend,
             const QString& teamName, QObject* owner,
             std::function<void()> onChanged)
{
    if (!owner || teamName.isEmpty()) {
        return;
    }
    const QSet<QString> slugs = unresolvedSlugs(document, teamName);
    QPointer<QTextDocument> doc(&document);
    QPointer<QObject> guard(owner);
    auto& service = ChannelReferenceService::instance(backend);
    for (const QString& slug : slugs) {
        service.resolve(teamName, slug,
            [doc, guard, slug, teamName, onChanged](
                const QString& displayName) {
            if (!guard || !doc) {
                return;
            }
            if (replace(*doc, teamName, slug, displayName) && onChanged) {
                onChanged();
            }
        });
    }
}

} // namespace ChannelReferenceDisplayNames
} // namespace Mattermost
