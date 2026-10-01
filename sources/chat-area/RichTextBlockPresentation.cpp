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
#include <QPointer>
#include <QScrollBar>
#include <QStringList>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>
#include <QWidget>

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
constexpr char DecorationOverlayName[] = "_matterleast_rich_block_decoration_overlay";
constexpr char CodeWidgetBorderName[] = "_matterleast_code_widget_border_overlay";

QColor codeBackground()
{
    // Keep the composer and the standalone received-post code widget on the
    // same Monokai-derived surface that MatterLeast already uses for code.
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

bool isCodeBlock(const QTextBlock& block)
{
    if (!block.isValid()) {
        return false;
    }
    const QTextBlockFormat format = block.blockFormat();
    return format.nonBreakableLines()
        || format.hasProperty(QTextFormat::BlockCodeFence)
        || format.hasProperty(QTextFormat::BlockCodeLanguage);
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

class BlockDecorationOverlay final : public QWidget
{
public:
    explicit BlockDecorationOverlay(QTextEdit* editor)
        : QWidget(editor ? editor->viewport() : nullptr)
        , editor_(editor)
    {
        setObjectName(QString::fromLatin1(DecorationOverlayName));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setFocusPolicy(Qt::NoFocus);
        syncGeometry();
        show();
        raise();
    }

    void syncGeometry()
    {
        if (!editor_ || !editor_->viewport()) {
            return;
        }
        const QRect wanted = editor_->viewport()->rect();
        if (geometry() != wanted) {
            setGeometry(wanted);
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (!editor_ || !richPresentationEnabled(editor_)) {
            return;
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        drawCodeGroups(painter);
        drawQuoteGroups(painter);
    }

private:
    template<typename Predicate, typename PaintGroup>
    void drawGroups(QPainter& painter,
                    Predicate predicate,
                    PaintGroup paintGroup)
    {
        QTextDocument* document = editor_ ? editor_->document() : nullptr;
        if (!document) {
            return;
        }

        bool active = false;
        QRectF group;
        const auto flush = [&] {
            if (active && group.isValid() && group.bottom() >= 0
                && group.top() <= height()) {
                paintGroup(painter, group);
            }
            active = false;
            group = {};
        };

        for (QTextBlock block = document->begin(); block.isValid(); block = block.next()) {
            if (!predicate(block)) {
                flush();
                continue;
            }

            const QRectF current = blockViewportRect(*editor_, block);
            if (!active) {
                active = true;
                group = current;
            } else {
                group = group.united(current);
            }
        }
        flush();
    }

    void drawCodeGroups(QPainter& painter)
    {
        drawGroups(
            painter,
            [](const QTextBlock& block) { return isCodeBlock(block); },
            [this](QPainter& groupPainter, QRectF rect) {
                rect.setLeft(0.5);
                rect.setRight(std::max<qreal>(0.5, width() - 0.5));
                rect.adjust(0.0, -0.5, 0.0, 0.5);
                groupPainter.setBrush(Qt::NoBrush);
                groupPainter.setPen(QPen(codeBorder(), 1.0));
                groupPainter.drawRoundedRect(
                    rect, CodeBorderRadius, CodeBorderRadius);
            });
    }

    void drawQuoteGroups(QPainter& painter)
    {
        drawGroups(
            painter,
            [](const QTextBlock& block) { return isQuoteBlock(block); },
            [this](QPainter& groupPainter, QRectF rect) {
                const QRectF barRect(
                    QuoteBarOffset,
                    rect.top(),
                    QuoteBarWidth,
                    std::max<qreal>(1.0, rect.height()));
                groupPainter.setPen(Qt::NoPen);
                groupPainter.setBrush(quoteBar(editor_->palette()));
                groupPainter.drawRoundedRect(barRect, 1.5, 1.5);
            });
    }

    QPointer<QTextEdit> editor_;
};

class CodeWidgetBorderOverlay final : public QWidget
{
public:
    explicit CodeWidgetBorderOverlay(QPlainTextEdit* editor)
        : QWidget(editor)
        , editor_(editor)
    {
        setObjectName(QString::fromLatin1(CodeWidgetBorderName));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setFocusPolicy(Qt::NoFocus);
        syncGeometry();
        show();
        raise();
    }

    void syncGeometry()
    {
        if (!editor_) {
            return;
        }
        if (geometry() != editor_->rect()) {
            setGeometry(editor_->rect());
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (!editor_) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(codeBorder(), 1.0));
        QRectF border = rect();
        border.adjust(0.5, 0.5, -0.5, -0.5);
        painter.drawRoundedRect(border, CodeBorderRadius, CodeBorderRadius);
    }

private:
    QPointer<QPlainTextEdit> editor_;
};

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

        // Quotes first; code second so a code block nested in a quote keeps the
        // code palette while the quote bar still remains visible.
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
            if (!isCodeBlock(block)) {
                continue;
            }
            QTextEdit::ExtraSelection selection;
            selection.cursor = QTextCursor(block);
            selection.cursor.clearSelection();
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

    if (auto* overlay = editor->viewport()->findChild<BlockDecorationOverlay*>(
            QString::fromLatin1(DecorationOverlayName),
            Qt::FindDirectChildrenOnly)) {
        overlay->syncGeometry();
        overlay->raise();
    }
}

void ensureTextEditPresentation(QTextEdit* editor)
{
    if (!editor || editor->property(PresentationHookProperty).toBool()) {
        return;
    }
    editor->setProperty(PresentationHookProperty, true);

    if (qobject_cast<MessageTextEditWidget*>(editor)) {
        // This changes only QTextDocument layout geometry. The logical list
        // level remains QTextListFormat::indent(), and Markdown serialization
        // is therefore unchanged.
        editor->document()->setIndentWidth(ComposerListIndentWidth);
    }

    auto* overlay = new BlockDecorationOverlay(editor);

    QObject::connect(editor, &QTextEdit::textChanged, editor, [editor, overlay] {
        updatePresentationSelections(editor);
        if (overlay) {
            overlay->syncGeometry();
        }
    });
    QObject::connect(editor->verticalScrollBar(), &QScrollBar::valueChanged,
                     editor, [overlay](int) {
        if (overlay) {
            overlay->syncGeometry();
        }
    });
    QObject::connect(editor->horizontalScrollBar(), &QScrollBar::valueChanged,
                     editor, [overlay](int) {
        if (overlay) {
            overlay->syncGeometry();
        }
    });

    if (auto* composer = qobject_cast<MessageTextEditWidget*>(editor)) {
        QObject::connect(
            composer, &MessageTextEditWidget::editingModeChanged,
            composer, [editor, overlay](MessageTextEditWidget::EditingMode) {
                updatePresentationSelections(editor);
                if (overlay) {
                    overlay->syncGeometry();
                }
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
    editor->setPalette(palette);

    // Do not delegate code-block boundaries to the current platform style.
    // Some styles collapse the QPlainTextEdit frame entirely; use the same
    // explicit one-pixel block boundary as the composer instead.
    editor->setFrameShape(QFrame::NoFrame);
    new CodeWidgetBorderOverlay(editor);
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
                if (auto* overlay = composer->viewport()->findChild<BlockDecorationOverlay*>(
                        QString::fromLatin1(DecorationOverlayName),
                        Qt::FindDirectChildrenOnly)) {
                    overlay->syncGeometry();
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
                if (auto* overlay = browser->viewport()->findChild<BlockDecorationOverlay*>(
                        QString::fromLatin1(DecorationOverlayName),
                        Qt::FindDirectChildrenOnly)) {
                    overlay->syncGeometry();
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
                if (wanted != code->palette()) {
                    code->setPalette(wanted);
                }
                if (auto* overlay = code->findChild<CodeWidgetBorderOverlay*>(
                        QString::fromLatin1(CodeWidgetBorderName),
                        Qt::FindDirectChildrenOnly)) {
                    overlay->syncGeometry();
                }
            }
            return false;
        }

        // QAbstractScrollArea's viewport can resize independently when a
        // scrollbar appears/disappears. Keep decoration overlays exactly over
        // the viewport instead of assuming the outer QTextEdit resized too.
        if (type == QEvent::Resize) {
            if (auto* widget = qobject_cast<QWidget*>(watched)) {
                if (auto* editor = qobject_cast<QTextEdit*>(widget->parentWidget())) {
                    if (widget == editor->viewport()) {
                        if (auto* overlay = widget->findChild<BlockDecorationOverlay*>(
                                QString::fromLatin1(DecorationOverlayName),
                                Qt::FindDirectChildrenOnly)) {
                            overlay->syncGeometry();
                        }
                    }
                }
            }
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
} // namespace Mattermost

static void installMatterLeastRichTextBlockPresentation()
{
    Mattermost::installRichTextBlockPresentation();
}

Q_COREAPP_STARTUP_FUNCTION(installMatterLeastRichTextBlockPresentation)
