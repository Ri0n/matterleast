#include "PostForwardPolicy.h"

#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

#include "backend/types/BackendChannel.h"

namespace Mattermost {

// Match Mattermost's forwarding privacy boundary: a permalink from a public
// channel may be shared elsewhere, while a private channel or DM/GM may only
// forward inside its original conversation.
bool canForwardPostTo(int sourceType,
                             const QString& sourceChannelId,
                             const QString& targetChannelId)
{
    return !sourceChannelId.isEmpty() && !targetChannelId.isEmpty()
        && (sourceType == BackendChannel::publicChannel
            || sourceChannelId == targetChannelId);
}

// The server recognizes permalinks only under its configured SiteURL and the
// native /<team-slug>/pl/<26-char-post-id> path. The connection host and
// /_redirect/pl/<id> are not necessarily recognized, so never substitute them.
QString nativeForwardPermalink(QString siteUrl,
                                      const QString& teamSlug,
                                      const QString& postId)
{
    static const QRegularExpression teamPattern(
        QStringLiteral("^[0-9a-z_-]{1,64}$"));
    static const QRegularExpression postPattern(
        QStringLiteral("^[a-z0-9]{26}$"));
    siteUrl = siteUrl.trimmed();
    const QUrl url(siteUrl, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme() != QLatin1String("https")
            && url.scheme() != QLatin1String("http"))
        || !url.userInfo().isEmpty() || !url.query().isEmpty()
        || !url.fragment().isEmpty()
        || !teamPattern.match(teamSlug).hasMatch()
        || !postPattern.match(postId).hasMatch()) {
        return {};
    }
    while (siteUrl.endsWith(QLatin1Char('/'))) {
        siteUrl.chop(1);
    }
    return siteUrl + QLatin1Char('/') + teamSlug
        + QStringLiteral("/pl/") + postId;
}

// Only suppress a technical permalink when the server supplied an authorized
// preview for that exact post. An absent/denied embed must leave the link
// visible rather than misleadingly pretending that forwarding succeeded.
QString textWithPermalinkPreview(const QString& message,
                                       const QJsonArray& embeds)
{
    const int newline = message.lastIndexOf(QLatin1Char('\n'));
    const QString trailingLine = message.mid(newline + 1).trimmed();
    const QUrl url(trailingLine, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()
        || (url.scheme() != QLatin1String("https")
            && url.scheme() != QLatin1String("http"))) {
        return message;
    }

    for (const QJsonValue& embedValue : embeds) {
        const QJsonObject embed = embedValue.toObject();
        if (embed.value(QStringLiteral("type")).toString()
            != QLatin1String("permalink")) {
            continue;
        }
        const QJsonObject data = embed.value(QStringLiteral("data")).toObject();
        const QJsonObject preview = data.value(QStringLiteral("post")).toObject();
        if (preview.isEmpty()) {
            // Preview content withheld by the server: do not reveal anything.
            continue;
        }
        QString postId = data.value(QStringLiteral("post_id")).toString();
        if (postId.isEmpty()) {
            postId = preview.value(QStringLiteral("id")).toString();
        }
        if (!postId.isEmpty()
            && url.path().endsWith(QStringLiteral("/pl/") + postId)) {
            return newline < 0 ? QString() : message.left(newline);
        }
    }
    return message;
}

// Forwarding is a normal post whose last line is the original permalink.
// The server generates its own permission-gated permalink preview.
QString forwardedPostText(const QString& comment, const QString& permalink)
{
    if (permalink.isEmpty()) {
        return {};
    }
    return comment.trimmed().isEmpty()
        ? permalink : comment + QLatin1Char('\n') + permalink;
}

} // namespace Mattermost
