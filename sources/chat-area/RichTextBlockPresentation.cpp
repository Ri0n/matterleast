/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 *
 * Presentation-only styling for structural Markdown blocks in the composer.
 * Rendered messages keep their existing QuoteBlock/CodeBlockEdit presentation;
 * this layer must not restyle those widgets globally.
 */

#include "RichTextBlockPresentation.h"

#include <algorithm>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QPainter>
#include <QPalette>
#include <QScrollBar>
#include <QStringList>
#include <QTextBlock>
#include <QTextBlockFormat>
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
constexpr char DecorationProperty[] = "_matterleast_composer_block_decoration";
constexpr char QuoteDecorationName[] = "_matterleast_composer_quote_bar";
constexpr char CodeDecorationName[] = "_matterleast_composer_code_border";

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
    const auto* composer = qobject_cast<const MessageTextEditWidget*>(editor);
    return composer && composer->isRichTextEditing();
}

QRectF blockViewportRect(QTextEdit& editor, const QTextBlock& block)
{
    if (!block.isValid() || !editor.viewport()) {
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

template<typename Predicate, typename VisitGroup>
void visitBlockGroups(QTextEdit& editor,
                      Predicate predicate,
                      VisitGroup visitGroup)
{
    QTextDocument* document = editor.document();
    if (!document || !editor.viewport()) {
        return;
    }

    bool active = false;
    QRectF group;
    const auto flush = [&] {
        if (active && group.isValid()) {
            visitGroup(group);
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

class ComposerBlockDecoration final : public QWidget
{
public:
    enum class Kind {
        QuoteBar,
        CodeBorder,
    };

    ComposerBlockDecoration(Kind kind, QWidget* parent)
        : QWidget(parent)
        , kind_(kind)
    {
        setProperty(DecorationProperty, true);
        setObjectName(QString::fromLatin1(
            kind_ == Kind::QuoteBar ? QuoteDecorationName : CodeDecorationName));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFocusPolicy(Qt::NoFocus);
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        if (kind_ == Kind::QuoteBar) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(palette().color(QPalette::Mid));
            painter.drawRoundedRect(rect(), 1.5, 1.5);
            return;
        }

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(codeBorder(), 1.0));
        QRectF border = rect();
        border.adjust(0.5, 0.5, -0.5, -0.5);
        painter.drawRoundedRect(border, CodeBorderRadius, CodeBorderRadius);
    }

private:
    Kind kind_;
};

void clearDecorationWidgets(QTextEdit* editor)
{
    if (!editor || !editor->viewport()) {
        return;
    }

    const auto children = editor->viewport()->findChildren<QWidget*>(
        QString(), Qt::FindDirectChildrenOnly);
    for (QWidget* child : children) {
        if (child && child->property(DecorationProperty).toBool()) {
            delete child;
        }
    }
}

QRect clippedGroupGeometry(QTextEdit& editor, QRectF group)
{
    if (!editor.viewport()) {
        return {};
    }
    QRect geometry = group.toAlignedRect();
    geometry.setLeft(0);
    geometry.setRight(std::max(0, editor.viewport()->width() - 1));
    return geometry.intersected(editor.viewport()->rect());
}

void syncDecorationWidgets(QTextEdit* editor)
{
    clearDecorationWidgets(editor);
    if (!richPresentationEnabled(editor) || !editor->viewport()) {
        return;
    }

    visitBlockGroups(
        *editor,
        [](const QTextBlock& block) { return isStructuralCodeBlock(block); },
        [editor](QRectF group) {
            const QRect geometry = clippedGroupGeometry(*editor, group);
            if (!geometry.isValid() || geometry.isEmpty()) {
                return;
            }
            auto* border = new ComposerBlockDecoration(
                ComposerBlockDecoration::Kind::CodeBorder, editor->viewport());
            border->setGeometry(geometry);
            border->show();
            border->raise();
        });

    visitBlockGroups(
        *editor,
        [](const QTextBlock& block) { return isQuoteBlock(block); },
        [editor](QRectF group) {
            const QRect clipped = clippedGroupGeometry(*editor, group);
            if (!clipped.isValid() || clipped.isEmpty()) {
                return;
            }
            auto* bar = new ComposerBlockDecoration(
                ComposerBlockDecoration::Kind::QuoteBar, editor->viewport());
            bar->setGeometry(
                QuoteBarOffset, clipped.top(), QuoteBarWidth, clipped.height());
            bar->show();
            bar->raise();
        });
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
}

void syncComposerPresentation(QTextEdit* editor)
{
    updatePresentationSelections(editor);
    syncDecorationWidgets(editor);
}

void ensureTextEditPresentation(MessageTextEditWidget* editor)
{
    if (!editor || editor->property(PresentationHookProperty).toBool()) {
        return;
    }
    editor->setProperty(PresentationHookProperty, true);

    // QTextDocument::indentWidth() controls layout only. It does not change
    // QTextListFormat::indent(), so Markdown nesting remains structural.
    editor->document()->setIndentWidth(ComposerListIndentWidth);

    QObject::connect(editor, &QTextEdit::textChanged, editor, [editor] {
        syncComposerPresentation(editor);
    });
    QObject::connect(
        editor, &MessageTextEditWidget::editingModeChanged,
        editor, [editor](MessageTextEditWidget::EditingMode) {
            syncComposerPresentation(editor);
        });
    QObject::connect(editor->verticalScrollBar(), &QScrollBar::valueChanged,
                     editor, [editor](int) {
        syncDecorationWidgets(editor);
    });
    QObject::connect(editor->verticalScrollBar(), &QScrollBar::rangeChanged,
                     editor, [editor](int, int) {
        syncDecorationWidgets(editor);
    });

    syncComposerPresentation(editor);
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
                syncComposerPresentation(composer);
            }
            return false;
        }

        // The viewport can resize independently when the vertical scrollbar is
        // shown or hidden. Keep the small decoration widgets pinned to the new
        // viewport geometry without changing rendered-message widgets.
        if (type == QEvent::Resize) {
            if (auto* viewport = qobject_cast<QWidget*>(watched)) {
                auto* composer = qobject_cast<MessageTextEditWidget*>(
                    viewport->parentWidget());
                if (composer && viewport == composer->viewport()) {
                    syncDecorationWidgets(composer);
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

void paintRichTextBlockDecorations(QTextEdit& editor, QPainter& painter)
{
    // Composer chrome is represented by small child widgets so it is not
    // dependent on QTextEdit's paint-event dirty region. Keep this legacy
    // entry point inert; rendered messages have their own presentation.
    Q_UNUSED(editor)
    Q_UNUSED(painter)
}

} // namespace Mattermost

static void installMatterLeastRichTextBlockPresentation()
{
    Mattermost::installRichTextBlockPresentation();
}

Q_COREAPP_STARTUP_FUNCTION(installMatterLeastRichTextBlockPresentation)
