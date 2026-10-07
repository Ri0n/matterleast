#include "EmojiRegistry.h"

#include <algorithm>

#include <QDir>
#include <QUrl>

#include "EmojiInfo.h"

namespace Mattermost {

EmojiRegistry::EmojiRegistry(QObject* parent)
    : QObject(parent)
{
    // Register generated image-backed built-ins (currently :mattermost:) as
    // custom presentation paths too, so sizing code does not need special cases.
    const QVector<Emoji> builtInCustom =
        EmojiInfo::getAllBuiltInEmojis(EmojiCategory::custom, 0);
    for (const Emoji& emoji : builtInCustom) {
        static const QString prefix = QStringLiteral(" <img src=\"");
        const int start = emoji.unicodeString.indexOf(prefix);
        if (start < 0) {
            continue;
        }
        const int pathStart = start + prefix.size();
        const int pathEnd = emoji.unicodeString.indexOf(QLatin1Char('"'), pathStart);
        if (pathEnd <= pathStart) {
            continue;
        }
        const QString normalizedPath =
            normalizedCustomEmojiPath(emoji.unicodeString.mid(
                pathStart, pathEnd - pathStart));
        if (!normalizedPath.isEmpty()) {
            _builtInCustomEmojiPaths.insert(normalizedPath);
        }
    }
}

std::optional<Emoji> EmojiRegistry::resolveByName(const QString& emojiName)
{
    if (const auto builtIn = EmojiInfo::resolveBuiltInByName(emojiName)) {
        return builtIn;
    }

    const auto customIt = _customEmojiPathsByName.constFind(emojiName);
    if (customIt != _customEmojiPathsByName.cend()) {
        return customEmojiPresentation(emojiName, customIt.value());
    }

    if (isValidCustomEmojiName(emojiName)) {
        emit customEmojiRequested(emojiName);
    }
    return std::nullopt;
}

QVector<Emoji> EmojiRegistry::getAllEmojis(
    uint32_t category, uint32_t skinTone) const
{
    QVector<Emoji> result =
        EmojiInfo::getAllBuiltInEmojis(category, skinTone);
    if (category != EmojiCategory::custom || _customEmojiPathsByName.isEmpty()) {
        return result;
    }

    QStringList names = _customEmojiPathsByName.keys();
    std::sort(names.begin(), names.end());
    result.reserve(result.size() + names.size());
    for (const QString& name : names) {
        result.push_back(
            customEmojiPresentation(name, _customEmojiPathsByName.value(name)));
    }
    return result;
}

void EmojiRegistry::addCustomEmoji(
    const QString& emojiName, const QString& emojiPath)
{
    if (!isValidCustomEmojiName(emojiName) || emojiPath.isEmpty()) {
        return;
    }

    // Generated built-ins remain authoritative if a server happens to return
    // the same short name.
    if (EmojiInfo::resolveBuiltInByName(emojiName)) {
        return;
    }

    const QString normalizedPath = normalizedCustomEmojiPath(emojiPath);
    if (normalizedPath.isEmpty()) {
        return;
    }

    const auto existing = _customEmojiPathsByName.constFind(emojiName);
    if (existing != _customEmojiPathsByName.cend()) {
        if (existing.value() == normalizedPath) {
            return;
        }

        const QString oldPath = existing.value();
        bool oldPathStillUsed = false;
        for (auto it = _customEmojiPathsByName.cbegin();
             it != _customEmojiPathsByName.cend(); ++it) {
            if (it.key() != emojiName && it.value() == oldPath) {
                oldPathStillUsed = true;
                break;
            }
        }
        if (!oldPathStillUsed) {
            _customEmojiPaths.remove(oldPath);
        }
    }

    _customEmojiPathsByName.insert(emojiName, normalizedPath);
    _customEmojiPaths.insert(normalizedPath);
    emit customEmojiAdded(emojiName);
}

void EmojiRegistry::clearCustomEmojis()
{
    _customEmojiPathsByName.clear();
    _customEmojiPaths.clear();
}

bool EmojiRegistry::isCustomEmojiPath(const QString& emojiPath) const
{
    const QString normalizedPath = normalizedCustomEmojiPath(emojiPath);
    return !normalizedPath.isEmpty()
        && (_customEmojiPaths.contains(normalizedPath)
            || _builtInCustomEmojiPaths.contains(normalizedPath));
}

bool EmojiRegistry::isValidCustomEmojiName(const QString& name)
{
    if (name.isEmpty()) {
        return false;
    }

    for (const QChar character : name) {
        const ushort value = character.unicode();
        const bool asciiLetter = (value >= 'A' && value <= 'Z')
            || (value >= 'a' && value <= 'z');
        const bool asciiDigit = value >= '0' && value <= '9';
        if (!asciiLetter && !asciiDigit
            && character != QLatin1Char('_')
            && character != QLatin1Char('-')
            && character != QLatin1Char('+')) {
            return false;
        }
    }
    return true;
}

QString EmojiRegistry::normalizedCustomEmojiPath(QString path)
{
    if (path.isEmpty()) {
        return {};
    }

    const QUrl url(path);
    if (url.isLocalFile()) {
        path = url.toLocalFile();
    }
    return QDir::cleanPath(QDir::fromNativeSeparators(path));
}

Emoji EmojiRegistry::customEmojiPresentation(
    const QString& name, const QString& path)
{
    // The 1px dimensions are a serialization sentinel. Renderers replace them
    // with font-relative custom-emoji metrics before display.
    return Emoji {
        name,
        QStringLiteral(" <img src=\"%1\" width=1 height=1> ")
            .arg(path.toHtmlEscaped())
    };
}

} // namespace Mattermost
