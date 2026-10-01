/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#pragma once

class QKeyEvent;
class QTextEdit;

namespace Mattermost::RichTextEditorCommands {

enum class ListStyle {
    Bullet,
    Numbered,
};

bool toggleList(QTextEdit& editor, ListStyle style);
bool toggleQuote(QTextEdit& editor);
bool toggleCodeBlock(QTextEdit& editor);

// Kept as the list-specific primitive while the aggregate structural handler
// composes list/quote/code/table behavior around it.
bool handleListKey(QTextEdit& editor, QKeyEvent& event);

/**
 * Handle structural rich-text keys before the composer's submit-on-Enter
 * policy. Returns true only when the key has been fully consumed.
 */
bool handleStructuralKey(QTextEdit& editor, QKeyEvent& event);

} // namespace Mattermost::RichTextEditorCommands
