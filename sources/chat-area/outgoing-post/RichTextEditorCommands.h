/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#pragma once

#include <QTextListFormat>

class QKeyEvent;
class QTextEdit;

namespace Mattermost::RichTextEditorCommands {

enum class ListStyle {
    Bullet,
    Numbered,
};

/** Toggle/convert the selected rich-text blocks to the requested list style. */
bool toggleList(QTextEdit& editor, ListStyle style);

/**
 * Handle structural list keys before the composer's submit-on-Enter policy.
 * Returns true only when the key has been fully consumed.
 */
bool handleListKey(QTextEdit& editor, QKeyEvent& event);

} // namespace Mattermost::RichTextEditorCommands
