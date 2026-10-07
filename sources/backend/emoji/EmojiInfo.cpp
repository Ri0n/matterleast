/**
 * @file EmojiInfo.cpp
 * @brief Contains functions for getting emoji by ID and adding custom emojis
 * @author Lyubomir Filipov
 * @date Dec 30, 2022
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "EmojiInfo.h"

#include <QDebug>
#include <QDir>
#include <QMap>
#include <QSet>
#include <QUrl>

#include "EmojiRegistryNotifier.h"

namespace Mattermost {

extern QVector<Emoji> emojiVecNoSkinVariadic[EmojiCategory::COUNT];
extern QVector<SkinVariadicEmoji> emojiVecSkinVariadic;
extern QMap<QString, EmojiMapEntry> emojiMap;

namespace {

QSet<QString> customEmojiPaths;
QVector<Emoji> dynamicCustomEmojis;
QMap<QString, int> dynamicCustomEmojiIndexes;

bool isValidCustomEmojiName(const QString& name)
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

void requestCustomEmoji(const QString& name)
{
    if (isValidCustomEmojiName(name)) {
        emit EmojiRegistryNotifier::instance().customEmojiRequested(name);
    }
}

QString normalizedCustomEmojiPath(QString path)
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

} // namespace

/**
 * Search for a skin tone string in the emoji name. Remove it, when performing lookup,
 * because emoji names are stored without the skin tone. All skin tone emojis are in an array and
 * the lookup is performed without the skin tone name.
 *
 * Also, the skin tone lookup order is changed, so that searching for 'light' will not find 'medium_light'
 */
static QString skinTonelookup[] {"", "medium_light", "medium_dark", "light", "medium", "dark"};
static const uint16_t skinToneLookupMap[] {
		EmojiSkinTone::none,
		EmojiSkinTone::mediumLight,
		EmojiSkinTone::mediumDark,
		EmojiSkinTone::light,
		EmojiSkinTone::medium,
		EmojiSkinTone::dark,
};

namespace {

std::optional<Emoji> resolveBuiltinEmoji(const EmojiMapEntry& entry, uint16_t skinTone)
{
    if (entry.kind == EmojiMapEntry::Kind::nonSkinVariadic) {
        if (entry.category >= EmojiCategory::COUNT) {
            return std::nullopt;
        }

        const auto& emojis = emojiVecNoSkinVariadic[entry.category];
        if (entry.index >= emojis.size()) {
            return std::nullopt;
        }
        return emojis[entry.index];
    }

    if (entry.kind != EmojiMapEntry::Kind::skinVariadic
        || entry.index >= emojiVecSkinVariadic.size()
        || skinTone >= EmojiSkinTone::COUNT) {
        return std::nullopt;
    }

    const SkinVariadicEmoji& variadicEmoji = emojiVecSkinVariadic[entry.index];
    if (skinTone >= variadicEmoji.unicodeString.size()) {
        return std::nullopt;
    }

    QString emojiName = variadicEmoji.name;
    if (skinTone != EmojiSkinTone::none) {
        emojiName += QStringLiteral(" (skin tone: %1)")
                         .arg(skinTonelookup[skinTone]);
    }

    return Emoji {emojiName, variadicEmoji.unicodeString[skinTone]};
}

} // namespace

std::optional<Emoji> EmojiInfo::resolveByName(const QString& emojiName)
{
    const auto customIt = dynamicCustomEmojiIndexes.constFind(emojiName);
    if (customIt != dynamicCustomEmojiIndexes.cend()) {
        const int index = customIt.value();
        if (index >= 0 && index < dynamicCustomEmojis.size()) {
            return dynamicCustomEmojis[index];
        }
        return std::nullopt;
    }

    QString lookupName = emojiName;
    uint16_t skinTone = EmojiSkinTone::none;
    for (uint16_t i = 1; i < EmojiSkinTone::COUNT; ++i) {
        const QString suffix = QStringLiteral("_")
            + skinTonelookup[i]
            + QStringLiteral("_skin_tone");
        const int found = lookupName.indexOf(suffix);
        if (found != -1) {
            lookupName.remove(found, suffix.size());
            skinTone = skinToneLookupMap[i];
            break;
        }
    }

    const auto it = emojiMap.constFind(lookupName);
    if (it == emojiMap.cend()) {
        requestCustomEmoji(emojiName);
        return std::nullopt;
    }

    return resolveBuiltinEmoji(it.value(), skinTone);
}

QVector<Emoji> EmojiInfo::getAllEmojis(uint32_t category, uint32_t skinTone)
{
    if (category >= EmojiCategory::COUNT || skinTone >= EmojiSkinTone::COUNT) {
        return {};
    }

    QVector<Emoji> result(emojiVecNoSkinVariadic[category]);

    if (category == EmojiCategory::people) {
        for (const auto& emoji : emojiVecSkinVariadic) {
            if (skinTone < static_cast<uint32_t>(emoji.unicodeString.size())) {
                result.push_back(Emoji {emoji.name, emoji.unicodeString[skinTone]});
            }
        }
    } else if (category == EmojiCategory::custom) {
        result += dynamicCustomEmojis;
    }

    return result;
}

void EmojiInfo::addCustomEmoji(const QString& emojiName, const QString& emojiPath)
{
    const QString normalizedPath = normalizedCustomEmojiPath(emojiPath);

    const auto dynamicIt = dynamicCustomEmojiIndexes.constFind(emojiName);
    if (dynamicIt != dynamicCustomEmojiIndexes.cend()) {
        if (!normalizedPath.isEmpty()) {
            customEmojiPaths.insert(normalizedPath);
        }
        return;
    }

    // The built-in "mattermost" image lives in the generated custom category.
    // Keep it authoritative instead of shadowing it with a runtime entry.
    const auto builtinIt = emojiMap.constFind(emojiName);
    if (builtinIt != emojiMap.cend()
        && builtinIt.value().kind == EmojiMapEntry::Kind::nonSkinVariadic
        && builtinIt.value().category == EmojiCategory::custom) {
        if (!normalizedPath.isEmpty()) {
            customEmojiPaths.insert(normalizedPath);
        }
        return;
    }

    const Emoji emoji {
        emojiName,
        QStringLiteral(" <img src=\"%1\" width=1 height=1> ")
            .arg(emojiPath.toHtmlEscaped())
    };

    const int index = dynamicCustomEmojis.size();
    dynamicCustomEmojis.push_back(emoji);
    dynamicCustomEmojiIndexes.insert(emojiName, index);

    if (!normalizedPath.isEmpty()) {
        customEmojiPaths.insert(normalizedPath);
    }

    emit EmojiRegistryNotifier::instance().customEmojiAdded(emojiName);
}

bool EmojiInfo::isCustomEmojiPath(const QString& emojiPath)
{
    const QString normalizedPath = normalizedCustomEmojiPath(emojiPath);
    return !normalizedPath.isEmpty() && customEmojiPaths.contains(normalizedPath);
}

} /* namespace Mattermost */