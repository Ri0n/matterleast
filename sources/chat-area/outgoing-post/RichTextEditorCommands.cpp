/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 */

#include "RichTextEditorCommands.h"

#include <algorithm>

#include <QDebug>
#include <QKeyEvent>
#include <QStringList>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>
#include <QTextList>

namespace Mattermost::RichTextEditorCommands {
namespace {

struct BlockRange {
    QTextBlock first;
    QTextBlock last;
};

struct ListMove {
    QTextList* source = nullptr;
    QTextListFormat format;
    QVector<QTextBlock> blocks;
    int targetIndent = 1;
};

struct ListDetach {
    QTextList* source = nullptr;
    QTextListFormat format;
    QVector<QTextBlock> selected;
    QVector<QTextBlock> rightSide;
    bool hasLeftSide = false;
};

QTextListFormat::Style qtStyle(ListStyle style)
{
    return style == ListStyle::Numbered
        ? QTextListFormat::ListDecimal
        : QTextListFormat::ListDisc;
}

int listIndent(const QTextBlock& block)
{
    if (QTextList* list = block.textList()) {
        return std::max(1, list->format().indent());
    }
    return 0;
}

QTextListFormat::Style listStyle(const QTextBlock& block)
{
    // All callers establish list membership first. Keep a valid Qt enum value
    // as the defensive fallback because QTextListFormat::Style has no
    // "undefined" enumerator on either Qt 5 or Qt 6.
    if (QTextList* list = block.textList()) {
        return list->format().style();
    }
    return QTextListFormat::ListDisc;
}

void traceListState(const char* action, const QTextEdit& editor)
{
    const QTextDocument* document = editor.document();
    if (!document) {
        return;
    }

    QStringList blocks;
    int index = 0;
    for (QTextBlock block = document->begin(); block.isValid(); block = block.next(), ++index) {
        QString kind = QStringLiteral("plain");
        if (QTextList* list = block.textList()) {
            kind = QStringLiteral("list(style=%1,indent=%2,count=%3,obj=%4)")
                       .arg(static_cast<int>(list->format().style()))
                       .arg(list->format().indent())
                       .arg(list->count())
                       .arg(block.blockFormat().objectIndex());
        }
        QString text = block.text();
        text.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
        blocks.push_back(
            QStringLiteral("#%1{%2,text=\"%3\"}").arg(index).arg(kind, text));
    }

    QString markdown = document->toMarkdown(QTextDocument::MarkdownDialectGitHub);
    markdown.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    qDebug().noquote()
        << "RICH_TEXT_LIST"
        << "action=" << action
        << "indentWidth=" << document->indentWidth()
        << "markdown=\"" + markdown + "\""
        << "blocks=" + blocks.join(QStringLiteral(" "));
}

BlockRange selectedBlockRange(QTextEdit& editor)
{
    const QTextCursor cursor = editor.textCursor();
    const int start = cursor.selectionStart();
    int end = cursor.selectionEnd();
    if (cursor.hasSelection() && end > start) {
        // A selection ending exactly at the next block boundary belongs to the
        // previous selected block, not to the untouched following paragraph.
        --end;
    }

    BlockRange range;
    range.first = editor.document()->findBlock(start);
    range.last = editor.document()->findBlock(std::max(start, end));
    return range;
}

QVector<QTextBlock> blocksInRange(const BlockRange& range)
{
    QVector<QTextBlock> blocks;
    if (!range.first.isValid() || !range.last.isValid()) {
        return blocks;
    }

    for (QTextBlock block = range.first; block.isValid(); block = block.next()) {
        blocks.push_back(block);
        if (block == range.last) {
            break;
        }
    }
    return blocks;
}

QVector<QTextBlock> selectedListBlocksWithTrailingSubtree(QTextEdit& editor)
{
    QVector<QTextBlock> blocks = blocksInRange(selectedBlockRange(editor));
    if (blocks.isEmpty()) {
        return blocks;
    }
    if (std::any_of(blocks.cbegin(), blocks.cend(), [](const QTextBlock& block) {
            return block.textList() == nullptr;
        })) {
        return {};
    }

    const int lastIndent = listIndent(blocks.constLast());
    for (QTextBlock block = blocks.constLast().next(); block.isValid(); block = block.next()) {
        if (!block.textList() || listIndent(block) <= lastIndent) {
            break;
        }
        blocks.push_back(block);
    }
    return blocks;
}

QTextCursor fullBlockCursor(QTextDocument* document,
                            const QTextBlock& first,
                            const QTextBlock& last)
{
    QTextCursor cursor(document);
    cursor.setPosition(first.position());
    cursor.setPosition(
        last.position() + std::max(0, last.length() - 1),
        QTextCursor::KeepAnchor);
    return cursor;
}

void normalizeDetachedBlock(const QTextBlock& block)
{
    QTextCursor cursor(block);
    QTextBlockFormat format = block.blockFormat();
    format.setObjectIndex(-1);
    format.setIndent(0);
    cursor.setBlockFormat(format);
}

QVector<ListDetach> detachGroups(const QVector<QTextBlock>& blocks)
{
    QVector<ListDetach> groups;
    for (const QTextBlock& block : blocks) {
        QTextList* source = block.textList();
        if (!source) {
            continue;
        }
        auto it = std::find_if(groups.begin(), groups.end(), [source](const ListDetach& group) {
            return group.source == source;
        });
        if (it == groups.end()) {
            ListDetach group;
            group.source = source;
            group.format = source->format();
            groups.push_back(group);
            it = std::prev(groups.end());
        }
        it->selected.push_back(block);
    }

    // Capture the right-hand members before changing list membership. QTextList
    // may delete itself as soon as its final item is removed.
    for (ListDetach& group : groups) {
        int firstIndex = group.source->count();
        int lastIndex = -1;
        for (const QTextBlock& block : group.selected) {
            const int index = group.source->itemNumber(block);
            firstIndex = std::min(firstIndex, index);
            lastIndex = std::max(lastIndex, index);
        }
        group.hasLeftSide = firstIndex > 0;
        for (int index = lastIndex + 1; index < group.source->count(); ++index) {
            group.rightSide.push_back(group.source->item(index));
        }
    }
    return groups;
}

void detachBlocksFromLists(const QVector<QTextBlock>& blocks)
{
    const QVector<ListDetach> groups = detachGroups(blocks);
    for (const ListDetach& group : groups) {
        for (const QTextBlock& block : group.selected) {
            // remove() also clears the list object index. It may destroy the
            // QTextList after the final member, so never dereference source
            // again after this loop unless a left/right side guarantees life.
            if (QTextList* list = block.textList()) {
                list->remove(block);
            }
            normalizeDetachedBlock(block);
        }

        // Removing an item in the middle must produce list / paragraph / list,
        // not one QTextList object whose items silently jump across the plain
        // paragraph. Re-home the right side in a fresh list object.
        if (group.hasLeftSide && !group.rightSide.isEmpty()) {
            QTextCursor rightCursor(group.rightSide.constFirst());
            QTextList* rightList = rightCursor.createList(group.format);
            for (int index = 1; index < group.rightSide.size(); ++index) {
                rightList->add(group.rightSide.at(index));
            }
        }
    }
}

bool selectionIsRequestedList(const QVector<QTextBlock>& blocks, ListStyle style)
{
    if (blocks.isEmpty()) {
        return false;
    }
    const auto requested = qtStyle(style);
    return std::all_of(blocks.cbegin(), blocks.cend(), [requested](const QTextBlock& block) {
        return block.textList() && listStyle(block) == requested;
    });
}

void applyListStyle(QTextEdit& editor,
                    const QVector<QTextBlock>& blocks,
                    ListStyle style)
{
    if (blocks.isEmpty()) {
        return;
    }

    const auto requested = qtStyle(style);
    int groupStart = 0;
    while (groupStart < blocks.size()) {
        const int indent = std::max(1, listIndent(blocks.at(groupStart)));
        int groupEnd = groupStart;
        while (groupEnd + 1 < blocks.size()
               && blocks.at(groupEnd).next() == blocks.at(groupEnd + 1)
               && std::max(1, listIndent(blocks.at(groupEnd + 1))) == indent) {
            ++groupEnd;
        }

        QTextListFormat format;
        format.setStyle(requested);
        format.setIndent(indent);
        QTextCursor cursor = fullBlockCursor(
            editor.document(), blocks.at(groupStart), blocks.at(groupEnd));
        cursor.createList(format);
        groupStart = groupEnd + 1;
    }
}

QTextList* neighboringTargetList(const QVector<QTextBlock>& blocks,
                                 int targetIndent,
                                 QTextListFormat::Style style)
{
    if (blocks.isEmpty()) {
        return nullptr;
    }

    for (QTextBlock block = blocks.constFirst().previous();
         block.isValid() && block.textList(); block = block.previous()) {
        const int indent = listIndent(block);
        if (indent < targetIndent) {
            break;
        }
        if (indent == targetIndent && listStyle(block) == style) {
            return block.textList();
        }
    }

    for (QTextBlock block = blocks.constLast().next();
         block.isValid() && block.textList(); block = block.next()) {
        const int indent = listIndent(block);
        if (indent < targetIndent) {
            break;
        }
        if (indent == targetIndent && listStyle(block) == style) {
            return block.textList();
        }
    }
    return nullptr;
}

bool canIndent(const QVector<QTextBlock>& blocks)
{
    if (blocks.isEmpty()) {
        return false;
    }
    const int currentIndent = listIndent(blocks.constFirst());
    for (QTextBlock block = blocks.constFirst().previous();
         block.isValid() && block.textList(); block = block.previous()) {
        const int indent = listIndent(block);
        if (indent < currentIndent) {
            return false;
        }
        if (indent == currentIndent) {
            return true;
        }
    }
    return false;
}

bool changeListIndent(QTextEdit& editor, int delta)
{
    QVector<QTextBlock> blocks = selectedListBlocksWithTrailingSubtree(editor);
    if (blocks.isEmpty()) {
        return false;
    }

    if (delta > 0 && !canIndent(blocks)) {
        return true; // structural Tab is consumed even when indentation is invalid
    }
    if (delta < 0 && listIndent(blocks.constFirst()) <= 1) {
        return true;
    }

    const QTextCursor original = editor.textCursor();
    QTextCursor transaction = original;
    transaction.beginEditBlock();

    QVector<ListMove> moves;
    for (const QTextBlock& block : blocks) {
        QTextList* source = block.textList();
        if (!source) {
            continue;
        }
        auto it = std::find_if(moves.begin(), moves.end(), [source](const ListMove& move) {
            return move.source == source;
        });
        if (it == moves.end()) {
            ListMove move;
            move.source = source;
            move.format = source->format();
            move.targetIndent = std::max(1, source->format().indent() + delta);
            moves.push_back(move);
            it = std::prev(moves.end());
        }
        it->blocks.push_back(block);
    }

    for (const ListMove& move : moves) {
        QTextList* target = neighboringTargetList(
            move.blocks, move.targetIndent, move.format.style());
        if (!target) {
            QTextListFormat targetFormat = move.format;
            targetFormat.setIndent(move.targetIndent);
            QTextCursor first(move.blocks.constFirst());
            target = first.createList(targetFormat);
        } else {
            target->add(move.blocks.constFirst());
        }
        for (int index = 1; index < move.blocks.size(); ++index) {
            target->add(move.blocks.at(index));
        }
    }

    transaction.endEditBlock();
    editor.setTextCursor(original);
    traceListState(delta > 0 ? "indent" : "outdent", editor);
    return true;
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

bool splitListItem(QTextEdit& editor)
{
    QTextCursor cursor = editor.textCursor();
    const QTextBlock block = cursor.block();
    if (!block.textList()) {
        return false;
    }

    const int indent = listIndent(block);
    if (block.text().isEmpty()) {
        if (indent > 1) {
            return changeListIndent(editor, -1);
        }

        cursor.beginEditBlock();
        detachBlocksFromLists({block});
        cursor.endEditBlock();
        editor.setTextCursor(QTextCursor(block));
        traceListState("exit-empty-top-level", editor);
        return true;
    }

    QTextBlockFormat blockFormat = cursor.blockFormat();
    QTextCharFormat charFormat = cursor.charFormat();
    clearInheritedLink(charFormat);

    cursor.beginEditBlock();
    cursor.insertBlock(blockFormat, charFormat);
    cursor.endEditBlock();
    editor.setTextCursor(cursor);
    editor.setCurrentCharFormat(charFormat);
    traceListState("split", editor);
    return true;
}

bool insertListHardBreak(QTextEdit& editor)
{
    QTextCursor cursor = editor.textCursor();
    if (!cursor.block().textList()) {
        return false;
    }
    QTextCharFormat format = cursor.charFormat();
    clearInheritedLink(format);
    cursor.insertText(QString(QChar::LineSeparator), format);
    editor.setTextCursor(cursor);
    editor.setCurrentCharFormat(format);
    traceListState("hard-break", editor);
    return true;
}

bool backspaceAtListBoundary(QTextEdit& editor)
{
    QTextCursor cursor = editor.textCursor();
    if (cursor.hasSelection() || cursor.positionInBlock() != 0) {
        return false;
    }
    const QTextBlock block = cursor.block();
    if (!block.textList()) {
        return false;
    }

    if (listIndent(block) > 1) {
        return changeListIndent(editor, -1);
    }

    if (!block.text().isEmpty()) {
        cursor.beginEditBlock();
        detachBlocksFromLists({block});
        cursor.endEditBlock();
        editor.setTextCursor(QTextCursor(block));
        traceListState("backspace-unlist", editor);
        return true;
    }

    QTextList* list = block.textList();
    if (list && list->count() <= 1) {
        cursor.beginEditBlock();
        detachBlocksFromLists({block});
        cursor.endEditBlock();
        editor.setTextCursor(QTextCursor(block));
        traceListState("backspace-empty-only-item", editor);
        return true;
    }

    // An empty item in a multi-item top-level list is removed rather than
    // merely converted to a blank paragraph. Detach it first so deleting the
    // adjacent paragraph separator cannot absorb the neighbor into this list.
    cursor.beginEditBlock();
    detachBlocksFromLists({block});
    cursor = QTextCursor(block);
    if (block.previous().isValid()) {
        cursor.deletePreviousChar();
    } else if (block.next().isValid()) {
        cursor.deleteChar();
    }
    cursor.endEditBlock();
    editor.setTextCursor(cursor);
    traceListState("backspace-remove-empty-item", editor);
    return true;
}

bool deleteAtListBoundary(QTextEdit& editor)
{
    QTextCursor cursor = editor.textCursor();
    if (cursor.hasSelection() || cursor.positionInBlock() != cursor.block().length() - 1) {
        return false;
    }
    const QTextBlock block = cursor.block();
    if (!block.textList()) {
        return false;
    }

    const QTextBlock next = block.next();
    if (!next.isValid()) {
        return false;
    }

    if (next.textList()) {
        if (listIndent(next) != listIndent(block)) {
            return false;
        }
    } else {
        const QTextBlockFormat format = next.blockFormat();
        if (format.intProperty(QTextFormat::BlockQuoteLevel) > 0
            || format.hasProperty(QTextFormat::BlockCodeFence)) {
            return false;
        }
    }

    cursor.beginEditBlock();
    cursor.deleteChar();
    cursor.endEditBlock();
    editor.setTextCursor(cursor);
    traceListState("delete-merge", editor);
    return true;
}

} // namespace

bool toggleList(QTextEdit& editor, ListStyle style)
{
    const QTextCursor original = editor.textCursor();
    const QVector<QTextBlock> blocks = blocksInRange(selectedBlockRange(editor));
    if (blocks.isEmpty()) {
        return false;
    }

    QTextCursor transaction = original;
    transaction.beginEditBlock();
    if (selectionIsRequestedList(blocks, style)) {
        detachBlocksFromLists(blocks);
    } else {
        applyListStyle(editor, blocks, style);
    }
    transaction.endEditBlock();
    editor.setTextCursor(original);
    traceListState(style == ListStyle::Numbered ? "toggle-numbered" : "toggle-bullet",
                   editor);
    return true;
}

bool handleListKey(QTextEdit& editor, QKeyEvent& event)
{
    QTextCursor cursor = editor.textCursor();
    if (!cursor.block().textList()) {
        return false;
    }

    const Qt::KeyboardModifiers modifiers = event.modifiers();
    const bool primaryBlocked = modifiers.testFlag(Qt::ControlModifier)
        || modifiers.testFlag(Qt::AltModifier)
        || modifiers.testFlag(Qt::MetaModifier);

    if ((event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter)
        && !primaryBlocked) {
        const bool handled = modifiers.testFlag(Qt::ShiftModifier)
            ? insertListHardBreak(editor)
            : splitListItem(editor);
        if (handled) {
            event.accept();
        }
        return handled;
    }

    if ((event.key() == Qt::Key_Tab || event.key() == Qt::Key_Backtab)
        && !primaryBlocked) {
        const bool outdent = event.key() == Qt::Key_Backtab
            || modifiers.testFlag(Qt::ShiftModifier);
        const bool handled = changeListIndent(editor, outdent ? -1 : 1);
        if (handled) {
            event.accept();
        }
        return handled;
    }

    if (modifiers != Qt::NoModifier) {
        return false;
    }

    bool handled = false;
    if (event.key() == Qt::Key_Backspace) {
        handled = backspaceAtListBoundary(editor);
    } else if (event.key() == Qt::Key_Delete) {
        handled = deleteAtListBoundary(editor);
    }
    if (handled) {
        event.accept();
    }
    return handled;
}

} // namespace Mattermost::RichTextEditorCommands
