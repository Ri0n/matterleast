/**
 * @file EmojiDefs.h
 * @brief Emoji common type definitions, used by EmojiInfo and EmojiMapGenerator
 * @author Lyubomir Filipov
 * @date Dec 19, 2022
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

#pragma once

#include <QVector>

#include <cstdint>

namespace Mattermost {

namespace EmojiCategory {
	enum type {
		smileys,
		people,
		component,
		nature,
		food,
		travel,
		activities,
		objects,
		symbols,
		flags,
		custom,
		COUNT,
	};
}

const QString categoryDisplayNames[] {
	"Smileys",
	"People",
	"Component",
	"Animals\nNature",
	"Food\nDrink",
	"Travel\nPlaces",
	"Activities",
	"Objects",
	"Symbols",
	"Flags",
	"Custom",
};

namespace EmojiSkinTone {
enum type {
	none,
	light,
	mediumLight,
	medium,
	mediumDark,
	dark,
	COUNT
};

/**
 * Name part for each skin tone - appended to the emoji name
 */
const QString nameString[] {
	"",
	"_light_skin_tone",
	"_medium_light_skin_tone",
	"_medium_skin_tone",
	"_medium_dark_skin_tone",
	"_dark_skin_tone"
};

/**
 * Description for each skin tone (displayed in tooltipe)
 */
const QString descriptionString[] {
	"Classic (yellow)",
	"Light",
	"Medium-Light",
	"Medium",
	"Medium-Dark",
	"Dark"
};
}

/**
 * Generated lookup coordinates for a built-in emoji.
 *
 * This is storage metadata, not an emoji identity: Mattermost identifies emoji
 * by name, and runtime custom emoji are intentionally kept outside this map.
 */
struct EmojiMapEntry {
    enum class Kind : uint8_t {
        nonSkinVariadic,
        skinVariadic,
    };

    Kind kind;
    uint16_t category;
    uint16_t index;
};

/**
 * Emojis from the 'people' category have skin-tone variations
 * Store the unicode string for each skin code
 * Just appending the skin code does not work, because it is not
 * always at the end (example: mx_claus_light_skin_tone)
 */
struct SkinVariadicEmoji {
	QString				name;
	QVector<QString>	unicodeString;
};

/**
 * Non-people emojis. They have no skin-tone variations
 */
struct NonSkinVariadicEmoji {
	QString		name;
	QString 	unicodeString;
};


using Emoji = NonSkinVariadicEmoji;

} /* namespace Mattermost */

