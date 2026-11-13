#pragma once

#include <QDateTime>
#include <QFont>
#include <QPainter>
#include <QPixmap>
#include <QUrl>

#include "backend/emoji/EmojiRegistry.h"
#include "backend/types/BackendUser.h"
#include "ui/EmojiFont.h"

namespace Mattermost::UserStatusEmoji {

// The custom status is already part of the ordinary Mattermost user profile.
// Keep it separate from the display name (used for search, sort and mentions).
inline bool isExpired(const BackendUser& user)
{
    const QString& until = user.props.customStatus.expiresAt;
    // Mattermost stores ExpiresAt as Go time.Time. A custom status that does
    // not expire can serialize the zero value as 0001-01-01T00:00:00Z.
    // Treat this sentinel as "no deadline", not as an expired status.
    if (until.isEmpty() || until.startsWith(QStringLiteral("0001-01-01"))) {
        return false;
    }
    const QDateTime deadline = QDateTime::fromString(until, Qt::ISODate);
    return deadline.isValid() && deadline <= QDateTime::currentDateTime();
}

inline QString name(const BackendUser& user)
{
    if (isExpired(user)) {
        return {};
    }
    QString value = user.props.customStatus.emoji.trimmed();
    if (value.size() >= 2 && value.startsWith(QLatin1Char(':'))
        && value.endsWith(QLatin1Char(':'))) {
        value = value.mid(1, value.size() - 2);
    }
    return value;
}

inline QString text(const BackendUser& user)
{
    return isExpired(user) ? QString() : user.props.customStatus.text;
}

inline QPixmap pixmap(EmojiRegistry& registry, const BackendUser& user,
                      int size, const QFont& baseFont)
{
    const QString emojiName = name(user);
    if (emojiName.isEmpty() || size <= 0) {
        return {};
    }
    const auto emoji = registry.resolveByName(emojiName);
    if (!emoji) {
        // CustomEmojiService resolves unknown server emoji asynchronously.
        return {};
    }

    const QString presentation = emoji->unicodeString.trimmed();
    const QString imagePrefix = QStringLiteral("src=\"");
    const int sourceStart = presentation.indexOf(imagePrefix);
    if (sourceStart >= 0) {
        const int begin = sourceStart + imagePrefix.size();
        const int end = presentation.indexOf(QLatin1Char('"'), begin);
        if (end <= begin) {
            return {};
        }
        QString source = presentation.mid(begin, end - begin);
        source.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        if (source.startsWith(QStringLiteral("file://"))) {
            source = QUrl(source).toLocalFile();
        }
        const QPixmap original(source);
        if (original.isNull()) {
            return {};
        }
        QPixmap result(size, size);
        result.fill(Qt::transparent);
        QPainter painter(&result);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const QPixmap scaled = original.scaled(
            size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        painter.drawPixmap((size - scaled.width()) / 2,
                           (size - scaled.height()) / 2, scaled);
        return result;
    }

    if (presentation.isEmpty()) {
        return {};
    }
    QPixmap result(size, size);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QFont font = EmojiFont::applySystemEmojiFamily(baseFont);
    font.setBold(false);
    font.setPixelSize(qMax(1, size - 1));
    painter.setFont(font);
    painter.drawText(QRect(0, 0, size, size),
                     Qt::AlignCenter, presentation);
    return result;
}

} // namespace Mattermost::UserStatusEmoji
