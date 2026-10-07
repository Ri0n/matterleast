/**
 * @file EmojiInfo.h
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

#pragma once

#include <QVector>

#include <optional>
#include "EmojiDefs.h"

namespace Mattermost {

class EmojiInfo {
public:
	EmojiInfo ();
	virtual ~EmojiInfo ();
public:

    /**
     * Resolve an emoji presentation by its Mattermost name.
     *
     * Runtime custom emoji are resolved by name and never receive a numeric ID.
     * Missing valid names trigger the lazy custom-emoji resolver.
     */
    static std::optional<Emoji> resolveByName(const QString& emojiName);

	static QVector<Emoji> getAllEmojis (uint32_t category, uint32_t skinTone);

	/**
	 * Add a custom emoji.
	 * Custom emojis use image instead of unicode codepoints.
	 * However, images can be represented as <img> tags in Qt's QLabel rich text.
	 * This allows custom emojis also to use strings as values
	 * @param emojiName emoji name
	 * @param emojiPath path to the emoji image
	 */
	static void addCustomEmoji (const QString& emojiName, const QString& emojiPath);

    /** Return whether an image resource belongs to the custom-emoji registry. */
    static bool isCustomEmojiPath(const QString& emojiPath);
};

} /* namespace Mattermost */
