/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#pragma once

#include <iterator>

#include <QString>
#include <QVector>

class QKeyEvent;
class QTextEdit;

namespace Mattermost::RichTextEditorCommands {

enum class InlineStyle {
    Bold,
    Italic,
    StrikeOut,
    Code,
};

enum class ListStyle {
    Bullet,
    Numbered,
};

bool toggleInline(QTextEdit& editor, InlineStyle style);
bool toggleList(QTextEdit& editor, ListStyle style);
bool toggleQuote(QTextEdit& editor);
bool toggleCodeBlock(QTextEdit& editor);

/** Current fenced-code language at the cursor, or empty for plain/non-code. */
QString codeBlockLanguageAt(const QTextEdit& editor);

/**
 * Set/clear the language for the complete contiguous fenced-code region that
 * contains the cursor. Returns false when the cursor is not in a code block.
 */
bool setCodeBlockLanguage(QTextEdit& editor, const QString& language);

// Kept as the list-specific primitive while the aggregate structural handler
// composes list/quote/code/table behavior around it.
bool handleListKey(QTextEdit& editor, QKeyEvent& event);

/**
 * Handle structural rich-text keys before the composer's submit-on-Enter
 * policy. Returns true only when the key has been fully consumed.
 */
bool handleStructuralKey(QTextEdit& editor, QKeyEvent& event);

} // namespace Mattermost::RichTextEditorCommands
