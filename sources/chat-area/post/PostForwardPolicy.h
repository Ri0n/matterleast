#pragma once

#include <QJsonArray>
#include <QString>

namespace Mattermost {

// Public-channel messages can be forwarded elsewhere; private/DM/GM posts
// can only be forwarded within their original conversation.
bool canForwardPostTo(int sourceType, const QString& sourceChannelId,
                      const QString& targetChannelId);

// Native server-embeddable canonical permalink. Never use the connection
// hostname or /_redirect/pl/; Mattermost matches the configured SiteURL.
QString nativeForwardPermalink(QString siteUrl, const QString& teamSlug,
                               const QString& postId);

// Only suppress the trailing link for an authorized server permalink embed.
QString textWithPermalinkPreview(const QString& message,
                                const QJsonArray& embeds);

QString forwardedPostText(const QString& comment, const QString& permalink);

} // namespace Mattermost
