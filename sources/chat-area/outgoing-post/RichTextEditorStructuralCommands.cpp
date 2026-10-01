/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#include "RichTextEditorCommands.h"

#include <algorithm>

#include <QFontDatabase>
#include <QKeyEvent>
#include <QStringList>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>
#include <QTextList>
#include <QVector>

#include "chat-area/CodeBlockSupport.h"

namespace Mattermost::RichTextEditorCommands {
namespace {

struct BlockRange {
    QTextBlock first;
    QTextBlock last;
};

BlockRange selectedBlockRange(QTextEdit& editor)
{
    const QTextCursor cursor = editor.textCursor();
    const int start = cursor.selectionStart();
    int end = cursor.selectionEnd();
    if (cursor.hasSelection() && end > start) {
        --end;
    }
    return {
        editor.document()->findBlock(start),
        editor.document()->findBlock(std::max(start, end)),
    };
}

QVector<QTextBlock> blocksInRange(const BlockRange& range)
{
    QVector<QTextBlock> result;
    if (!range.first.isValid() || !range.last.isValid()) {
        return result;
    }
    for (QTextBlock block = range.first; block.isValid(); block = block.next()) {
        result.push_back(block);
        if (block == range.last) {
            break;
        }
    }
    return result;
}

void clearInheritedLink(QTextCharFormat& format)
{
    if (!format.isAnchor()) {
        return;
    }
    format.setAnchor(false);
    format.setAnchorHref(QString());
    format.setFontUnderline(false);
}

void setFontFamilyCompat(QTextCharFormat& format, const QString& family)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    format.setFontFamilies(QStringList {family});
#else
    format.setFontFamily(family);
#endif
}

bool inlineStyleEnabled(const QTextCharFormat& format, InlineStyle style)
{
    switch (style) {
    case InlineStyle::Bold:
        return format.fontWeight() >= QFont::Bold;
    case InlineStyle::Italic:
        return format.fontItalic();
    case InlineStyle::StrikeOut:
        return format.fontStrikeOut();
    case InlineStyle::Code:
        return format.fontFixedPitch();
    }
    return false;
}

QTextCharFormat inlineToggleFormat(QTextEdit& editor,
                                   InlineStyle style,
                                   bool enabled)
{
    QTextCharFormat format;
    switch (style) {
    case InlineStyle::Bold:
        format.setFontWeight(enabled ? QFont::Bold : QFont::Normal);
        break;
    case InlineStyle::Italic:
        format.setFontItalic(enabled);
        break;
    case InlineStyle::StrikeOut:
        format.setFontStrikeOut(enabled);
        break;
    case InlineStyle::Code:
        format.setFontFixedPitch(enabled);
        setFontFamilyCompat(
            format,
            enabled
                ? QFontDatabase::systemFont(QFontDatabase::FixedFont).family()
                : editor.font().family());
        break;
    }
    return format;
}

bool wholeSelectionHasInlineStyle(QTextEdit& editor,
                                  const QTextCursor& selection,
                                  InlineStyle style)
{
    if (!selection.hasSelection()) {
        return inlineStyleEnabled(selection.charFormat(), style);
    }

    const int start = selection.selectionStart();
    const int end = selection.selectionEnd();
    bool sawText = false;
    for (int position = start; position < end; ++position) {
        const QChar character = editor.document()->characterAt(position);
        if (character == QChar::ParagraphSeparator) {
            continue;
        }
        sawText = true;
        QTextCursor probe(editor.document());
        probe.setPosition(position);
        probe.movePosition(QTextCursor::NextCharacter, QTextCursor::KeepAnchor);
        if (!inlineStyleEnabled(probe.charFormat(), style)) {
            return false;
        }
    }
    return sawText;
}

void applyBlockFormat(const QTextBlock& block, const QTextBlockFormat& format)
{
    QTextCursor cursor(block);
    cursor.setBlockFormat(format);
}

void applyBlockCharFormat(const QTextBlock& block,
                          const QTextCharFormat& format)
{
    if (block.length() <= 1) {
        QTextCursor cursor(block);
        cursor.setBlockCharFormat(format);
        return;
    }
    QTextCursor cursor(block);
    cursor.setPosition(block.position());
    cursor.setPosition(block.position() + block.length() - 1,
                       QTextCursor::KeepAnchor);
    cursor.mergeCharFormat(format);
}

bool isCodeBlock(const QTextBlock& block)
{
    return isStructuralCodeBlock(block);
}

int quoteLevel(const QTextBlock& block)
{
    return block.isValid()
        ? block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel)
        : 0;
}

bool handleQuoteKey(QTextEdit& editor, QKeyEvent& event)
{
    QTextCursor cursor = editor.textCursor();
    const QTextBlock block = cursor.block();
    if (quoteLevel(block) <= 0) {
        return false;
    }

    const Qt::KeyboardModifiers modifiers = event.modifiers();
    const bool primaryBlocked = modifiers.testFlag(Qt::ControlModifier)
        || modifiers.testFlag(Qt::AltModifier)
        || modifiers.testFlag(Qt::MetaModifier);

    if ((event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter)
        && !primaryBlocked) {
        QTextCharFormat charFormat = cursor.charFormat();
        clearInheritedLink(charFormat);
        if (modifiers.testFlag(Qt::ShiftModifier)) {
            cursor.insertText(QString(QChar::LineSeparator), charFormat);
        } else if (block.text().isEmpty()) {
            QTextBlockFormat format = block.blockFormat();
            format.clearProperty(QTextFormat::BlockQuoteLevel);
            cursor.setBlockFormat(format);
        } else {
            cursor.insertBlock(block.blockFormat(), charFormat);
        }
        editor.setTextCursor(cursor);
        editor.setCurrentCharFormat(charFormat);
        event.accept();
        return true;
    }

    if (modifiers == Qt::NoModifier
        && event.key() == Qt::Key_Backspace
        && !cursor.hasSelection()
        && cursor.positionInBlock() == 0) {
        QTextBlockFormat format = block.blockFormat();
        const int level = quoteLevel(block);
        if (level <= 1) {
            format.clearProperty(QTextFormat::BlockQuoteLevel);
        } else {
            format.setProperty(QTextFormat::BlockQuoteLevel, level - 1);
        }
        cursor.setBlockFormat(format);
        editor.setTextCursor(cursor);
        event.accept();
        return true;
    }

    return false;
}

struct CodeLineChange {
    int position = 0;
    int delta = 0;
};

bool handleCodeIndent(QTextEdit& editor, bool outdent)
{
    const BlockRange range = selectedBlockRange(editor);
    QVector<QTextBlock> blocks = blocksInRange(range);
    if (blocks.isEmpty()
        || std::any_of(blocks.cbegin(), blocks.cend(), [](const QTextBlock& block) {
            return !isCodeBlock(block);
        })) {
        return false;
    }

    const QTextCursor original = editor.textCursor();
    QVector<CodeLineChange> changes;
    changes.reserve(blocks.size());

    QTextCursor transaction = original;
    transaction.beginEditBlock();
    for (int index = blocks.size() - 1; index >= 0; --index) {
        const QTextBlock block = blocks.at(index);
        QTextCursor line(block);
        int delta = 0;
        if (!outdent) {
            line.insertText(QStringLiteral("    "));
            delta = 4;
        } else {
            const QString text = block.text();
            int remove = 0;
            if (text.startsWith(QLatin1Char('\t'))) {
                remove = 1;
            } else {
                while (remove < 4 && remove < text.size()
                       && text.at(remove) == QLatin1Char(' ')) {
                    ++remove;
                }
            }
            if (remove > 0) {
                line.setPosition(block.position());
                line.setPosition(block.position() + remove,
                                 QTextCursor::KeepAnchor);
                line.removeSelectedText();
                delta = -remove;
            }
        }
        if (delta != 0) {
            changes.push_back({block.position(), delta});
        }
    }
    transaction.endEditBlock();

    int anchor = original.anchor();
    int position = original.position();
    for (const CodeLineChange& change : changes) {
        if (change.position <= anchor) {
            anchor += change.delta;
        }
        if (change.position <= position) {
            position += change.delta;
        }
    }
    QTextCursor restored(editor.document());
    restored.setPosition(std::max(0, anchor));
    restored.setPosition(std::max(0, position), QTextCursor::KeepAnchor);
    editor.setTextCursor(restored);
    return true;
}

bool handleCodeKey(QTextEdit& editor, QKeyEvent& event)
{
    QTextCursor cursor = editor.textCursor();
    const QTextBlock block = cursor.block();
    if (!isCodeBlock(block)) {
        return false;
    }

    const Qt::KeyboardModifiers modifiers = event.modifiers();
    const bool primaryBlocked = modifiers.testFlag(Qt::ControlModifier)
        || modifiers.testFlag(Qt::AltModifier)
        || modifiers.testFlag(Qt::MetaModifier);

    if ((event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter)
        && !primaryBlocked) {
        QTextCharFormat charFormat = cursor.charFormat();
        clearInheritedLink(charFormat);
        cursor.insertBlock(block.blockFormat(), charFormat);
        editor.setTextCursor(cursor);
        editor.setCurrentCharFormat(charFormat);
        event.accept();
        return true;
    }

    if ((event.key() == Qt::Key_Tab || event.key() == Qt::Key_Backtab)
        && !primaryBlocked) {
        const bool outdent = event.key() == Qt::Key_Backtab
            || modifiers.testFlag(Qt::ShiftModifier);
        if (handleCodeIndent(editor, outdent)) {
            event.accept();
            return true;
        }
    }

    if (modifiers == Qt::NoModifier
        && event.key() == Qt::Key_Backspace
        && !cursor.hasSelection()
        && cursor.positionInBlock() == 0
        && block.text().isEmpty()) {
        QTextBlockFormat blockFormat = block.blockFormat();
        blockFormat.clearProperty(QTextFormat::BlockCodeFence);
        blockFormat.clearProperty(QTextFormat::BlockCodeLanguage);
        cursor.setBlockFormat(blockFormat);

        QTextCharFormat charFormat = cursor.charFormat();
        charFormat.setFontFixedPitch(false);
        setFontFamilyCompat(charFormat, editor.font().family());
        cursor.setBlockCharFormat(charFormat);
        editor.setCurrentCharFormat(charFormat);
        editor.setTextCursor(cursor);
        event.accept();
        return true;
    }

    return false;
}

} // namespace

bool toggleInline(QTextEdit& editor, InlineStyle style)
{
    QTextCursor cursor = editor.textCursor();
    const bool enabled = !wholeSelectionHasInlineStyle(editor, cursor, style);
    const QTextCharFormat format = inlineToggleFormat(editor, style, enabled);

    if (cursor.hasSelection()) {
        cursor.beginEditBlock();
        cursor.mergeCharFormat(format);
        cursor.endEditBlock();
        editor.setTextCursor(cursor);
    } else {
        editor.mergeCurrentCharFormat(format);
    }
    return true;
}

bool toggleQuote(QTextEdit& editor)
{
    const QTextCursor original = editor.textCursor();
    const QVector<QTextBlock> blocks = blocksInRange(selectedBlockRange(editor));
    if (blocks.isEmpty()) {
        return false;
    }

    const bool allQuoted = std::all_of(
        blocks.cbegin(), blocks.cend(), [](const QTextBlock& block) {
            return quoteLevel(block) > 0;
        });

    QTextCursor transaction = original;
    transaction.beginEditBlock();
    for (const QTextBlock& block : blocks) {
        QTextBlockFormat format = block.blockFormat();
        if (allQuoted) {
            format.clearProperty(QTextFormat::BlockQuoteLevel);
        } else {
            format.setProperty(QTextFormat::BlockQuoteLevel, 1);
        }
        applyBlockFormat(block, format);
    }
    transaction.endEditBlock();
    editor.setTextCursor(original);
    return true;
}

bool toggleCodeBlock(QTextEdit& editor)
{
    const QTextCursor original = editor.textCursor();
    const QVector<QTextBlock> blocks = blocksInRange(selectedBlockRange(editor));
    if (blocks.isEmpty()) {
        return false;
    }

    const bool allCode = std::all_of(
        blocks.cbegin(), blocks.cend(), [](const QTextBlock& block) {
            return isCodeBlock(block);
        });

    QTextCursor transaction = original;
    transaction.beginEditBlock();
    for (const QTextBlock& block : blocks) {
        QTextBlockFormat blockFormat = block.blockFormat();
        QTextCharFormat charFormat;
        if (allCode) {
            blockFormat.clearProperty(QTextFormat::BlockCodeFence);
            blockFormat.clearProperty(QTextFormat::BlockCodeLanguage);
            charFormat.setFontFixedPitch(false);
            setFontFamilyCompat(charFormat, editor.font().family());
        } else {
            // A fenced code block is an exclusive block-level structure in the
            // composer. Do not leave a QTextList object attached underneath it.
            if (QTextList* list = block.textList()) {
                list->remove(block);
                blockFormat = block.blockFormat();
                blockFormat.setIndent(0);
            }
            blockFormat.setProperty(QTextFormat::BlockCodeFence,
                                    QStringLiteral("```"));
            charFormat.setFontFixedPitch(true);
            setFontFamilyCompat(
                charFormat,
                QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
        }
        applyBlockFormat(block, blockFormat);
        applyBlockCharFormat(block, charFormat);
    }
    transaction.endEditBlock();
    editor.setTextCursor(original);
    return true;
}

bool handleStructuralKey(QTextEdit& editor, QKeyEvent& event)
{
    // Nested/combined structures use the most specific editing semantics first.
    if (handleListKey(editor, event)) {
        return true;
    }
    if (handleCodeKey(editor, event)) {
        return true;
    }
    return handleQuoteKey(editor, event);
}

} // namespace Mattermost::RichTextEditorCommands