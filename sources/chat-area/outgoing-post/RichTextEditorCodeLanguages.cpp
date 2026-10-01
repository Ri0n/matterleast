#include "RichTextEditorCommands.h"

#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextFormat>

#include "chat-area/CodeBlockSupport.h"

namespace Mattermost::RichTextEditorCommands {
namespace {

struct CodeBlockRange {
    QTextBlock first;
    QTextBlock last;
};

CodeBlockRange contiguousCodeBlockAt(const QTextEdit& editor)
{
    QTextBlock current = editor.textCursor().block();
    if (!isStructuralCodeBlock(current)) {
        return {};
    }

    QTextBlock first = current;
    while (first.previous().isValid()
           && isStructuralCodeBlock(first.previous())) {
        first = first.previous();
    }

    QTextBlock last = current;
    while (last.next().isValid()
           && isStructuralCodeBlock(last.next())) {
        last = last.next();
    }
    return {first, last};
}

} // namespace

QString codeBlockLanguageAt(const QTextEdit& editor)
{
    const QTextBlock block = editor.textCursor().block();
    if (!isStructuralCodeBlock(block)) {
        return {};
    }
    return codeBlockLanguage(block);
}

bool setCodeBlockLanguage(QTextEdit& editor, const QString& language)
{
    const CodeBlockRange range = contiguousCodeBlockAt(editor);
    if (!range.first.isValid() || !range.last.isValid()) {
        return false;
    }

    QString canonical;
    if (!language.trimmed().isEmpty()) {
        canonical = canonicalCodeBlockLanguage(language);
        if (canonical.isEmpty()) {
            return false;
        }
    }

    const QTextCursor original = editor.textCursor();
    QTextCursor transaction = original;
    transaction.beginEditBlock();

    for (QTextBlock block = range.first; block.isValid(); block = block.next()) {
        QTextCursor cursor(block);
        QTextBlockFormat format = block.blockFormat();
        if (canonical.isEmpty()) {
            format.clearProperty(QTextFormat::BlockCodeLanguage);
        } else {
            format.setProperty(QTextFormat::BlockCodeLanguage, canonical);
        }
        cursor.setBlockFormat(format);

        if (block == range.last) {
            break;
        }
    }

    transaction.endEditBlock();
    editor.setTextCursor(original);
    return true;
}

} // namespace Mattermost::RichTextEditorCommands
