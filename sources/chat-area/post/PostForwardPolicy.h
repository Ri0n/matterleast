#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
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

// Standalone copy mode has no URL: official clients receive normal Markdown.
QString forwardedCopyText(const QString& comment, const QString& original);
QJsonObject forwardedCopyProps(const QString& postId,
                               const QString& authorId,
                               const QString& teamId);

// Incoming standalone forwards are identified by their post props, not
// by the sending client's local forwarding preference.
struct ForwardedCopyInfo {
    bool isForwarded = false;
    QString originalPostId;
    QString originalAuthorId;
};
ForwardedCopyInfo forwardedCopyInfo(const QJsonValue& props);

} // namespace Mattermost
