#include "EmojiRegistry.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QUrl>

#include "EmojiInfo.h"

namespace Mattermost {
namespace {

constexpr int MaxRuntimeCustomEmojiEntries = 1024;

QString customEmojiDirectory(const QString& path)
{
    if (path.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(
        QDir::fromNativeSeparators(QFileInfo(path).absolutePath()));
}

} // namespace

EmojiRegistry::EmojiRegistry(QObject* parent)
    : QObject(parent)
    , _customEmojiPathsByName(MaxRuntimeCustomEmojiEntries)
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

    if (const QString* customPath = _customEmojiPathsByName.object(emojiName)) {
        return customEmojiPresentation(emojiName, *customPath);
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
        if (const QString* path = _customEmojiPathsByName.object(name)) {
            result.push_back(customEmojiPresentation(name, *path));
        }
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

    if (const QString* existing = _customEmojiPathsByName.object(emojiName)) {
        if (*existing == normalizedPath) {
            return;
        }
    }

    const QString directory = customEmojiDirectory(normalizedPath);
    if (!directory.isEmpty()) {
        // CustomEmojiService stores one backend/session under a dedicated
        // directory. Tracking directories instead of every cached file keeps
        // presentation classification valid even after the name->path LRU
        // evicts an entry that an already-rendered QTextDocument still uses.
        _customEmojiDirectories.insert(directory);
    }

    _customEmojiPathsByName.insert(
        emojiName, new QString(normalizedPath));
    emit customEmojiAdded(emojiName);
}

void EmojiRegistry::clearCustomEmojis()
{
    _customEmojiPathsByName.clear();
    _customEmojiDirectories.clear();
}

void EmojiRegistry::dropMissingCustomEmojiFiles()
{
    const QStringList names = _customEmojiPathsByName.keys();
    for (const QString& name : names) {
        const QString* path = _customEmojiPathsByName.object(name);
        if (path && !QFileInfo::exists(*path)) {
            _customEmojiPathsByName.remove(name);
        }
    }
}

bool EmojiRegistry::isCustomEmojiPath(const QString& emojiPath) const
{
    const QString normalizedPath = normalizedCustomEmojiPath(emojiPath);
    if (normalizedPath.isEmpty()) {
        return false;
    }
    if (_builtInCustomEmojiPaths.contains(normalizedPath)) {
        return true;
    }
    const QString directory = customEmojiDirectory(normalizedPath);
    return !directory.isEmpty()
        && _customEmojiDirectories.contains(directory);
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
