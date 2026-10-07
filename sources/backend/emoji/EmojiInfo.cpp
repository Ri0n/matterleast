/**
 * @file EmojiInfo.cpp
 * @brief Immutable generated built-in emoji lookup
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
#include <QMap>

namespace Mattermost {

extern QVector<Emoji> emojiVecNoSkinVariadic[EmojiCategory::COUNT];
extern QVector<SkinVariadicEmoji> emojiVecSkinVariadic;
extern QMap<QString, EmojiMapEntry> emojiMap;

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
        const int index = static_cast<int>(entry.index);
        if (index >= emojis.size()) {
            return std::nullopt;
        }
        return emojis[index];
    }

    const int index = static_cast<int>(entry.index);
    if (entry.kind != EmojiMapEntry::Kind::skinVariadic
        || index >= emojiVecSkinVariadic.size()
        || skinTone >= EmojiSkinTone::COUNT) {
        return std::nullopt;
    }

    const SkinVariadicEmoji& variadicEmoji = emojiVecSkinVariadic[index];
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

std::optional<Emoji> EmojiInfo::resolveBuiltInByName(const QString& emojiName)
{
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
        return std::nullopt;
    }

    return resolveBuiltinEmoji(it.value(), skinTone);
}

QVector<Emoji> EmojiInfo::getAllBuiltInEmojis(uint32_t category, uint32_t skinTone)
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
    }

    return result;
}

} /* namespace Mattermost */