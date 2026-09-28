#pragma once

#include <QString>

namespace Mattermost {

enum class UserGroupValidationError {
    None,
    EmptyDisplayName,
    EmptyMention,
    InvalidMention,
    ReservedMention,
    MissingMembers,
};

inline QString canonicalGroupMention(QString mention)
{
    mention = mention.trimmed();
    while (mention.startsWith(QLatin1Char('@'))) {
        mention.remove(0, 1);
    }
    return mention.toLower();
}

inline QString suggestedGroupMention(const QString& displayName)
{
    QString result;
    const QString lower = displayName.toLower();
    for (const QChar ch : lower) {
        const ushort u = ch.unicode();
        const bool asciiLetter = u >= 'a' && u <= 'z';
        const bool digit = u >= '0' && u <= '9';
        if (asciiLetter || digit || ch == QLatin1Char('.')
            || ch == QLatin1Char('-') || ch == QLatin1Char('_')) {
            result += ch;
        }
        if (result.size() >= 64) {
            break;
        }
    }
    return result;
}

inline UserGroupValidationError validateUserGroupEditor(
    const QString& displayName,
    const QString& mention,
    int memberCount,
    bool requireMembers)
{
    if (displayName.trimmed().isEmpty()) {
        return UserGroupValidationError::EmptyDisplayName;
    }

    const QString canonical = canonicalGroupMention(mention);
    if (canonical.isEmpty()) {
        return UserGroupValidationError::EmptyMention;
    }

    static const QStringList reserved {
        QStringLiteral("all"),
        QStringLiteral("channel"),
        QStringLiteral("here"),
    };
    if (reserved.contains(canonical)) {
        return UserGroupValidationError::ReservedMention;
    }

    if (canonical.size() > 64) {
        return UserGroupValidationError::InvalidMention;
    }
    for (const QChar ch : canonical) {
        const ushort u = ch.unicode();
        const bool asciiLetter = u >= 'a' && u <= 'z';
        const bool digit = u >= '0' && u <= '9';
        if (!asciiLetter && !digit && ch != QLatin1Char('.')
            && ch != QLatin1Char('-') && ch != QLatin1Char('_')) {
            return UserGroupValidationError::InvalidMention;
        }
    }

    if (requireMembers && memberCount <= 0) {
        return UserGroupValidationError::MissingMembers;
    }

    return UserGroupValidationError::None;
}

} // namespace Mattermost
