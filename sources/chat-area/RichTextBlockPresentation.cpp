/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 *
 * Presentation-only styling for structural Markdown blocks. Markdown remains
 * the canonical representation: this layer never mutates QTextBlock/QTextChar
 * formats, because doing so would make the rich composer look modified and
 * force an otherwise untouched source through QTextDocument::toMarkdown().
 */

#include "RichTextBlockPresentation.h"

#include <algorithm>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QLayout>
#include <QPainter>
#include <QPalette>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>
#include <QWidget>

#include "chat-area/CodeBlockSupport.h"
#include "chat-area/outgoing-post/MessageTextEditWidget.h"

namespace Mattermost {
namespace {

constexpr qreal ComposerListIndentWidth = 20.0;
constexpr int QuoteBarWidth = 3;
constexpr int QuoteBarOffset = 1;
constexpr qreal QuoteTextOpacity = 0.72;
constexpr qreal CodeBorderRadius = 4.0;
constexpr int PresentationSelectionProperty = QTextFormat::UserProperty + 317;
constexpr char PresentationHookProperty[] = "_matterleast_rich_block_presentation_hook";

QColor codeBackground()
{
    return QColor(39, 40, 34);
}

QColor codeForeground()
{
    return QColor(227, 226, 214);
}

QColor codeBorder()
{
    QColor color = codeForeground();
    color.setAlpha(88);
    return color;
}

QColor quoteBar(const QPalette& palette)
{
    return palette.color(QPalette::Mid);
}

QColor quoteText(const QPalette& palette)
{
    QColor color = palette.color(QPalette::Text);
    color.setAlphaF(color.alphaF() * QuoteTextOpacity);
    return color;
}

bool isQuoteBlock(const QTextBlock& block)
{
    return block.isValid()
        && block.blockFormat().intProperty(QTextFormat::BlockQuoteLevel) > 0;
}

bool richPresentationEnabled(const QTextEdit* editor)
{
    if (!editor) {
        return false;
    }
    if (const auto* composer = qobject_cast<const MessageTextEditWidget*>(editor)) {
        return composer->isRichTextEditing();
    }
    return editor->objectName() == QStringLiteral("messageRichText");
}

bool isReceivedQuoteBrowser(const QTextBrowser* browser)
{
    if (!browser || browser->objectName() != QStringLiteral("messageRichText")) {
        return false;
    }
    const QWidget* parent = browser->parentWidget();
    const QLayout* layout = parent ? parent->layout() : nullptr;
    if (!layout || layout->count() < 2) {
        return false;
    }

    for (int index = 0; index < layout->count(); ++index) {
        QWidget* sibling = layout->itemAt(index)->widget();
        if (sibling && sibling != browser
            && sibling->minimumWidth() == QuoteBarWidth
            && sibling->maximumWidth() == QuoteBarWidth) {
            return true;
        }
    }
    return false;
}

void syncReceivedQuotePalette(QTextBrowser* browser)
{
    if (!isReceivedQuoteBrowser(browser)) {
        return;
    }
    QPalette wanted = browser->palette();
    const QColor text = quoteText(browser->parentWidget()
                                      ? browser->parentWidget()->palette()
                                      : browser->palette());
    wanted.setColor(QPalette::Text, text);
    wanted.setColor(QPalette::WindowText, text);
    if (wanted != browser->palette()) {
        browser->setPalette(wanted);
    }
}

QRectF blockViewportRect(QTextEdit& editor, const QTextBlock& block)
{
    if (!block.isValid()) {
        return {};
    }

    QTextCursor first(block);
    first.movePosition(QTextCursor::StartOfBlock);
    QTextCursor last(block);
    last.movePosition(QTextCursor::EndOfBlock);

    const QRect firstRect = editor.cursorRect(first);
    const QRect lastRect = editor.cursorRect(last);
    const qreal top = std::min(firstRect.top(), lastRect.top());
    const qreal bottom = std::max(firstRect.bottom(), lastRect.bottom());
    return QRectF(0.0, top, editor.viewport()->width(),
                  std::max<qreal>(1.0, bottom - top + 1.0));
}

template<typename Predicate, typename PaintGroup>
void paintBlockGroups(QTextEdit& editor,
                      QPainter& painter,
                      Predicate predicate,
                      PaintGroup paintGroup)
{
    QTextDocument* document = editor.document();
    if (!document || !editor.viewport()) {
        return;
    }

    bool active = false;
    QRectF group;
    const auto flush = [&] {
        if (active && group.isValid() && group.bottom() >= 0
            && group.top() <= editor.viewport()->height()) {
            paintGroup(group);
        }
        active = false;
        group = {};
    };

    for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
        if (!predicate(block)) {
            flush();
            continue;
        }

        const QRectF current = blockViewportRect(editor, block);
        if (!active) {
            active = true;
            group = current;
        } else {
            group = group.united(current);
        }
    }
    flush();
}

void updatePresentationSelections(QTextEdit* editor)
{
    if (!editor) {
        return;
    }

    QList<QTextEdit::ExtraSelection> selections;
    const auto previous = editor->extraSelections();
    for (const QTextEdit::ExtraSelection& selection : previous) {
        if (!selection.format.boolProperty(PresentationSelectionProperty)) {
            selections.push_back(selection);
        }
    }

    if (richPresentationEnabled(editor)) {
        const QPalette palette = editor->palette();
        const QFont fixedFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);

        for (QTextBlock block = editor->document()->begin();
             block.isValid(); block = block.next()) {
            if (!isQuoteBlock(block)) {
                continue;
            }
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(block);
            selection.cursor.select(QTextCursor::BlockUnderCursor);
            selection.format.setForeground(quoteText(palette));
            selection.format.setProperty(PresentationSelectionProperty, true);
            selections.push_back(selection);
        }

        for (QTextBlock block = editor->document()->begin();
             block.isValid(); block = block.next()) {
            if (!isStructuralCodeBlock(block)) {
                continue;
            }
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(block);
            selection.cursor.select(QTextCursor::BlockUnderCursor);
            selection.format.setBackground(codeBackground());
            selection.format.setForeground(codeForeground());
            selection.format.setFontFixedPitch(true);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
            selection.format.setFontFamilies(QStringList {fixedFont.family()});
#else
            selection.format.setFontFamily(fixedFont.family());
#endif
            selection.format.setProperty(QTextFormat::FullWidthSelection, true);
            selection.format.setProperty(PresentationSelectionProperty, true);
            selections.push_back(selection);
        }
    }

    editor->setExtraSelections(selections);
    if (editor->viewport()) {
        editor->viewport()->update();
    }
}

void ensureTextEditPresentation(QTextEdit* editor)
{
    if (!editor || editor->property(PresentationHookProperty).toBool()) {
        return;
    }
    editor->setProperty(PresentationHookProperty, true);

    if (qobject_cast<MessageTextEditWidget*>(editor)) {
        // QTextDocument::indentWidth() controls layout only. It does not change
        // QTextListFormat::indent(), so Markdown nesting remains structural.
        editor->document()->setIndentWidth(ComposerListIndentWidth);
    }

    QObject::connect(editor, &QTextEdit::textChanged, editor, [editor] {
        updatePresentationSelections(editor);
    });

    if (auto* composer = qobject_cast<MessageTextEditWidget*>(editor)) {
        QObject::connect(
            composer, &MessageTextEditWidget::editingModeChanged,
            composer, [editor](MessageTextEditWidget::EditingMode) {
                updatePresentationSelections(editor);
            });
    }

    updatePresentationSelections(editor);
}

void ensureCodeWidgetPresentation(QPlainTextEdit* editor)
{
    if (!editor || editor->property(PresentationHookProperty).toBool()) {
        return;
    }
    editor->setProperty(PresentationHookProperty, true);

    QPalette palette = editor->palette();
    palette.setColor(QPalette::Base, codeBackground());
    palette.setColor(QPalette::Text, codeForeground());
    palette.setColor(QPalette::WindowText, codeBorder());
    editor->setPalette(palette);

    // Use QFrame's own chrome instead of a transparent child overlay. This is
    // reliable across X11/Wayland styles and keeps the boundary attached to the
    // actual received-post code widget.
    editor->setFrameShape(QFrame::Box);
    editor->setFrameShadow(QFrame::Plain);
    editor->setLineWidth(1);
}

class RichTextBlockPresentationFilter final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!watched || !event) {
            return false;
        }

        const QEvent::Type type = event->type();
        const bool structuralEvent = type == QEvent::Show
            || type == QEvent::Polish
            || type == QEvent::Resize
            || type == QEvent::FontChange
            || type == QEvent::PaletteChange
            || type == QEvent::ApplicationPaletteChange;

        if (auto* composer = qobject_cast<MessageTextEditWidget*>(watched)) {
            if (structuralEvent) {
                ensureTextEditPresentation(composer);
                if (type == QEvent::PaletteChange
                    || type == QEvent::ApplicationPaletteChange
                    || type == QEvent::FontChange) {
                    updatePresentationSelections(composer);
                }
            }
            return false;
        }

        if (auto* browser = qobject_cast<QTextBrowser*>(watched)) {
            if (browser->objectName() == QStringLiteral("messageRichText")
                && structuralEvent) {
                ensureTextEditPresentation(browser);
                syncReceivedQuotePalette(browser);
                if (type == QEvent::PaletteChange
                    || type == QEvent::ApplicationPaletteChange
                    || type == QEvent::FontChange) {
                    updatePresentationSelections(browser);
                }
            }
            return false;
        }

        if (auto* code = qobject_cast<QPlainTextEdit*>(watched)) {
            if (code->objectName() == QStringLiteral("messageCodeBlock")
                && structuralEvent) {
                ensureCodeWidgetPresentation(code);
                QPalette wanted = code->palette();
                wanted.setColor(QPalette::Base, codeBackground());
                wanted.setColor(QPalette::Text, codeForeground());
                wanted.setColor(QPalette::WindowText, codeBorder());
                if (wanted != code->palette()) {
                    code->setPalette(wanted);
                }
            }
            return false;
        }

        return false;
    }
};

void installRichTextBlockPresentation()
{
    if (!qApp) {
        return;
    }
    qApp->installEventFilter(new RichTextBlockPresentationFilter(qApp));
}

} // namespace

void paintRichTextBlockDecorations(QTextEdit& editor, QPainter& painter)
{
    if (!richPresentationEnabled(&editor) || !editor.viewport()) {
        return;
    }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    paintBlockGroups(
        editor, painter,
        [](const QTextBlock& block) { return isStructuralCodeBlock(block); },
        [&editor, &painter](QRectF rect) {
            rect.setLeft(0.5);
            rect.setRight(std::max<qreal>(0.5, editor.viewport()->width() - 0.5));
            rect.adjust(0.0, -0.5, 0.0, 0.5);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(codeBorder(), 1.0));
            painter.drawRoundedRect(rect, CodeBorderRadius, CodeBorderRadius);
        });

    paintBlockGroups(
        editor, painter,
        [](const QTextBlock& block) { return isQuoteBlock(block); },
        [&editor, &painter](QRectF rect) {
            const QRectF barRect(
                QuoteBarOffset,
                rect.top(),
                QuoteBarWidth,
                std::max<qreal>(1.0, rect.height()));
            painter.setPen(Qt::NoPen);
            painter.setBrush(quoteBar(editor.palette()));
            painter.drawRoundedRect(barRect, 1.5, 1.5);
        });

    painter.restore();
}

} // namespace Mattermost

static void installMatterLeastRichTextBlockPresentation()
{
    Mattermost::installRichTextBlockPresentation();
}

Q_COREAPP_STARTUP_FUNCTION(installMatterLeastRichTextBlockPresentation)
