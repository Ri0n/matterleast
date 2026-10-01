#pragma once

#include <algorithm>

#include <QAbstractAnimation>
#include <QDynamicPropertyChangeEvent>
#include <QEasingCurve>
#include <QEvent>
#include <QFont>
#include <QKeyEvent>
#include <QLayout>
#include <QPalette>
#include <QPointer>
#include <QPropertyAnimation>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFormat>
#include <QTextList>
#include <QTimer>
#include <QWidget>

namespace Mattermost {
namespace MessageTextEditWidgetInteractionDetail {

constexpr char EditingPostProperty[] = "_mmqt_editing_post";
constexpr char FormattingAnimationHookProperty[] =
    "_matterleast_formatting_animation_hook";
constexpr char FormattingAnimationObjectName[] =
    "_matterleast_formatting_toolbar_animation";
constexpr char FormattingRequestedVisibleProperty[] =
    "_matterleast_formatting_requested_visible";
constexpr char FormattingLayoutNormalizedProperty[] =
    "_matterleast_formatting_layout_normalized";
constexpr char PostEditModeHookProperty[] =
    "_matterleast_post_edit_mode_hook";
constexpr char MarkdownLinkHighlighterObjectName[] =
    "_matterleast_markdown_link_highlighter";
constexpr char StructuralPlaceholderSuppressedProperty[] =
    "_matterleast_structural_placeholder_suppressed";
constexpr char StructuralPlaceholderTextProperty[] =
    "_matterleast_structural_placeholder_text";
constexpr int FormattingToolbarAnimationMs = 110;
constexpr int FormattingToolbarGap = 2;
constexpr qreal FormattingToolbarContentScale = 1.25;

class MarkdownSourceLinkHighlighter final : public QSyntaxHighlighter
{
public:
    explicit MarkdownSourceLinkHighlighter(MessageTextEditWidget* editor)
        : QSyntaxHighlighter(editor)
        , editor_(editor)
    {
        setObjectName(QString::fromLatin1(MarkdownLinkHighlighterObjectName));
        if (editor_) {
            setDocument(editor_->document());
        }
    }

protected:
    void highlightBlock(const QString& text) override
    {
        if (!editor_) {
            return;
        }

        static const QRegularExpression expression(
            QStringLiteral("\\[([^\\]\\n]+)\\]\\((https?://[^)\\s]+)\\)"),
            QRegularExpression::CaseInsensitiveOption);

        QTextCharFormat linkFormat;
        linkFormat.setForeground(editor_->palette().color(QPalette::Link));
        linkFormat.setFontUnderline(true);

        auto matches = expression.globalMatch(text);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            // Keep the complete Markdown source visible and editable. Only the
            // human-readable label is decorated like a conventional link.
            setFormat(match.capturedStart(1),
                      match.capturedLength(1),
                      linkFormat);
        }
    }

private:
    QPointer<MessageTextEditWidget> editor_;
};

inline MarkdownSourceLinkHighlighter* markdownLinkHighlighterFor(
    MessageTextEditWidget* editor)
{
    if (!editor) {
        return nullptr;
    }

    QObject* object = editor->findChild<QObject*>(
        QString::fromLatin1(MarkdownLinkHighlighterObjectName),
        Qt::FindDirectChildrenOnly);
    return dynamic_cast<MarkdownSourceLinkHighlighter*>(object);
}

inline void setMarkdownLinkHighlighting(MessageTextEditWidget* editor,
                                        bool enabled)
{
    if (!editor) {
        return;
    }

    MarkdownSourceLinkHighlighter* highlighter =
        markdownLinkHighlighterFor(editor);
    if (enabled) {
        if (!highlighter) {
            highlighter = new MarkdownSourceLinkHighlighter(editor);
        }
        highlighter->rehighlight();
        return;
    }

    if (highlighter) {
        highlighter->setDocument(nullptr);
        delete highlighter;
    }
}

inline QWidget* formattingToolbarFor(MessageTextEditWidget* editor)
{
    if (!editor || !editor->parentWidget()) {
        return nullptr;
    }
    return editor->parentWidget()->findChild<QWidget*>(
        QStringLiteral("formattingToolbar"));
}

inline void syncFormattingToolbarFont(MessageTextEditWidget* editor,
                                      QWidget* toolbar)
{
    if (!editor || !toolbar) {
        return;
    }

    // Keep the toolbar tied to the user's current UI font/scale instead of
    // baking a point size into the .ui file. ThemeIconButton also derives its
    // normalized symbolic-icon extent from these font metrics, so text glyphs
    // and SVG icons grow together.
    QFont font = editor->font();
    if (font.pointSizeF() > 0.0) {
        font.setPointSizeF(font.pointSizeF() * FormattingToolbarContentScale);
    } else if (font.pixelSize() > 0) {
        font.setPixelSize(std::max(
            1, qRound(font.pixelSize() * FormattingToolbarContentScale)));
    }
    if (toolbar->font() != font) {
        toolbar->setFont(font);
    }
}

inline void normalizeFormattingToolbarLayout(MessageTextEditWidget* editor)
{
    QWidget* toolbar = formattingToolbarFor(editor);
    if (!editor || !toolbar) {
        return;
    }

    syncFormattingToolbarFont(editor, toolbar);

    // The reaction quick bar never removes its animated slot from layout: the
    // collapsed slot simply has maximumWidth=0. Do the vertical equivalent
    // here. uic's direct setVisible() connection would reintroduce a discrete
    // layout step, so animation exclusively owns the slot extent instead.
    QObject::disconnect(
        editor,
        &MessageTextEditWidget::formattingToolbarVisibilityChanged,
        toolbar,
        &QWidget::setVisible);

    if (toolbar->property(FormattingLayoutNormalizedProperty).toBool()) {
        return;
    }
    toolbar->setProperty(FormattingLayoutNormalizedProperty, true);

    // Like the quick bar's 1 px separator, the composer gap lives inside the
    // zero-height animated slot. It therefore grows continuously with the slot
    // instead of appearing atomically when a hidden widget becomes visible.
    if (QWidget* container = toolbar->parentWidget()) {
        if (QLayout* layout = container->layout()) {
            layout->setSpacing(0);
        }
    }
    if (QLayout* layout = toolbar->layout()) {
        QMargins margins = layout->contentsMargins();
        margins.setBottom(std::max(margins.bottom(), FormattingToolbarGap));
        layout->setContentsMargins(margins);
    }

    toolbar->setMinimumHeight(0);
    toolbar->setMaximumHeight(0);
    toolbar->show();
    toolbar->updateGeometry();
}

inline int formattingToolbarExpandedHeight(QWidget* toolbar)
{
    if (!toolbar) {
        return 0;
    }
    if (QLayout* layout = toolbar->layout()) {
        return std::max(1, layout->sizeHint().height());
    }
    return std::max(1, toolbar->sizeHint().height());
}

inline void animateFormattingToolbar(MessageTextEditWidget* editor, bool expand)
{
    QWidget* toolbar = formattingToolbarFor(editor);
    if (!toolbar) {
        return;
    }

    if (auto* previous = toolbar->findChild<QPropertyAnimation*>(
            QString::fromLatin1(FormattingAnimationObjectName),
            Qt::FindDirectChildrenOnly)) {
        previous->stop();
        previous->deleteLater();
    }

    int currentHeight = toolbar->maximumHeight();
    if (currentHeight >= QWIDGETSIZE_MAX) {
        currentHeight = toolbar->height();
    }
    const int endHeight = expand ? formattingToolbarExpandedHeight(toolbar) : 0;

    editor->setProperty(FormattingRequestedVisibleProperty, expand);
    if (currentHeight == endHeight) {
        toolbar->updateGeometry();
        return;
    }

    // Mirror ReactionQuickBarController::animateSlot(): animate the slot's
    // maximum extent, keep it in layout at zero when collapsed, and use the
    // same asymmetric easing for opening/closing.
    auto* animation = new QPropertyAnimation(
        toolbar, "maximumHeight", toolbar);
    animation->setObjectName(QString::fromLatin1(FormattingAnimationObjectName));
    animation->setDuration(FormattingToolbarAnimationMs);
    animation->setStartValue(std::max(0, currentHeight));
    animation->setEndValue(endHeight);
    animation->setEasingCurve(
        expand ? QEasingCurve::OutCubic : QEasingCurve::InCubic);

    QObject::connect(animation, &QPropertyAnimation::valueChanged,
                     toolbar, [toolbar](const QVariant&) {
        toolbar->updateGeometry();
    });
    QObject::connect(animation, &QPropertyAnimation::finished,
                     toolbar, [toolbar] {
        toolbar->updateGeometry();
    });

    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

inline void ensureFormattingAnimationHook(MessageTextEditWidget* editor)
{
    if (!editor) {
        return;
    }

    // This may run before or after uic creates its connections. Keep the
    // normalization idempotent and repeat the disconnect once the toolbar
    // exists, even when our animation hook was installed earlier.
    normalizeFormattingToolbarLayout(editor);

    if (editor->property(FormattingAnimationHookProperty).toBool()) {
        return;
    }

    editor->setProperty(FormattingAnimationHookProperty, true);
    QObject::connect(
        editor,
        &MessageTextEditWidget::formattingToolbarVisibilityChanged,
        editor,
        [editor](bool visible) {
            normalizeFormattingToolbarLayout(editor);
            animateFormattingToolbar(editor, visible);
        });
}

inline bool hasEmptyRichStructure(MessageTextEditWidget* editor)
{
    if (!editor || !editor->isRichTextEditing()
        || !editor->document()->toPlainText().isEmpty()) {
        return false;
    }

    const QTextBlock block = editor->textCursor().block();
    if (!block.isValid()) {
        return false;
    }

    if (block.textList()) {
        return true;
    }

    const QTextBlockFormat format = block.blockFormat();
    return format.intProperty(QTextFormat::BlockQuoteLevel) > 0
        || format.hasProperty(QTextFormat::BlockCodeFence);
}

inline void syncStructuralPlaceholder(MessageTextEditWidget* editor)
{
    if (!editor) {
        return;
    }

    const bool suppress = hasEmptyRichStructure(editor);
    const bool suppressed =
        editor->property(StructuralPlaceholderSuppressedProperty).toBool();
    if (suppress == suppressed) {
        return;
    }

    if (suppress) {
        // Flip the state first. setProperty() itself can synchronously deliver
        // QDynamicPropertyChangeEvent, and the re-entrant event must already
        // observe a consistent state instead of attempting suppression again.
        editor->setProperty(StructuralPlaceholderSuppressedProperty, true);
        editor->setProperty(
            StructuralPlaceholderTextProperty, editor->placeholderText());
        editor->setPlaceholderText(QString());
        return;
    }

    const QString placeholder =
        editor->property(StructuralPlaceholderTextProperty).toString();
    editor->setProperty(StructuralPlaceholderSuppressedProperty, false);
    editor->setProperty(StructuralPlaceholderTextProperty, QVariant());
    editor->setPlaceholderText(placeholder);
}

inline void ensurePostEditModeHook(MessageTextEditWidget* editor)
{
    if (!editor || editor->property(PostEditModeHookProperty).toBool()) {
        return;
    }

    editor->setProperty(PostEditModeHookProperty, true);
    QObject::connect(
        editor,
        &MessageTextEditWidget::editingModeChanged,
        editor,
        [editor](MessageTextEditWidget::EditingMode mode) {
            if (mode != MessageTextEditWidget::EditingMode::RichText
                || !editor->property(EditingPostProperty).toBool()) {
                return;
            }

            // Existing posts deliberately use the hybrid source presentation:
            // raw Markdown remains exact while links are only decorated. If a
            // generic mode action tries to enter full rich mode, return to the
            // source presentation on the next turn.
            QTimer::singleShot(0, editor, [editor] {
                if (editor->property(EditingPostProperty).toBool()
                    && editor->isRichTextEditing()) {
                    editor->setRichTextEditing(false);
                }
            });
        });
}

inline bool escapeRichLinkOnDoubleSpace(MessageTextEditWidget* editor,
                                        QKeyEvent* keyEvent)
{
    if (!editor || !keyEvent || !editor->isRichTextEditing()
        || keyEvent->key() != Qt::Key_Space
        || keyEvent->modifiers() != Qt::NoModifier) {
        return false;
    }

    QTextCursor cursor = editor->textCursor();
    if (cursor.hasSelection() || cursor.position() <= 0
        || !cursor.charFormat().isAnchor()) {
        return false;
    }

    QTextCursor previousCharacter = cursor;
    if (!previousCharacter.movePosition(
            QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor)
        || previousCharacter.selectedText() != QStringLiteral(" ")) {
        return false;
    }

    QTextCharFormat plainFormat = cursor.charFormat();
    plainFormat.setAnchor(false);
    plainFormat.setAnchorHref(QString());
    plainFormat.setFontUnderline(false);

    previousCharacter.removeSelectedText();
    previousCharacter.insertText(QStringLiteral(" "), plainFormat);
    editor->setTextCursor(previousCharacter);
    editor->setCurrentCharFormat(plainFormat);
    keyEvent->accept();
    return true;
}

inline bool escapeMarkdownLinkOnDoubleSpace(MessageTextEditWidget* editor,
                                            QKeyEvent* keyEvent)
{
    if (!editor || !keyEvent || editor->isRichTextEditing()
        || keyEvent->key() != Qt::Key_Space
        || keyEvent->modifiers() != Qt::NoModifier) {
        return false;
    }

    QTextCursor cursor = editor->textCursor();
    if (cursor.hasSelection() || cursor.position() <= 1) {
        return false;
    }

    const QString markdown = editor->markdownText();
    const int cursorPosition = cursor.position();
    if (cursorPosition > markdown.size()
        || markdown.at(cursorPosition - 1) != QLatin1Char(' ')) {
        return false;
    }

    static const QRegularExpression expression(
        QStringLiteral("\\[([^\\]\\n]+)\\]\\((https?://[^)\\s]+)\\)"),
        QRegularExpression::CaseInsensitiveOption);

    const int linkEnd = cursorPosition - 1;
    bool directlyAfterLink = false;
    auto matches = expression.globalMatch(markdown.left(linkEnd));
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        if (match.capturedEnd(0) == linkEnd) {
            directlyAfterLink = true;
        }
    }
    if (!directlyAfterLink) {
        return false;
    }

    QTextCursor previousCharacter = cursor;
    previousCharacter.movePosition(
        QTextCursor::PreviousCharacter, QTextCursor::KeepAnchor);
    previousCharacter.removeSelectedText();
    previousCharacter.insertText(QStringLiteral(" "));
    editor->setTextCursor(previousCharacter);
    keyEvent->accept();
    return true;
}

} // namespace MessageTextEditWidgetInteractionDetail

inline bool MessageTextEditWidget::event(QEvent* event)
{
    using namespace MessageTextEditWidgetInteractionDetail;

    // OutgoingPostCreator historically disabled the context menu. Restore the
    // ordinary QTextEdit policy without making that class own editor behavior.
    if (contextMenuPolicy() != Qt::DefaultContextMenu) {
        setContextMenuPolicy(Qt::DefaultContextMenu);
    }

    ensureFormattingAnimationHook(this);
    ensurePostEditModeHook(this);
    syncStructuralPlaceholder(this);

    if (event && event->type() == QEvent::DynamicPropertyChange) {
        auto* propertyEvent = static_cast<QDynamicPropertyChangeEvent*>(event);
        if (propertyEvent->propertyName() == EditingPostProperty) {
            const bool editing = property(EditingPostProperty).toBool();
            if (editing && !postEditModeForced_) {
                postEditModeForced_ = true;
                restoreRichAfterPostEdit_ = isRichTextEditing();
                if (restoreRichAfterPostEdit_) {
                    setRichTextEditing(false);
                }
                setMarkdownLinkHighlighting(this, true);
            } else if (!editing && postEditModeForced_) {
                setMarkdownLinkHighlighting(this, false);
                const bool restoreRich = restoreRichAfterPostEdit_;
                postEditModeForced_ = false;
                restoreRichAfterPostEdit_ = false;
                if (restoreRich) {
                    setRichTextEditing(true);
                }
            }
        }
    }

    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange)) {
        if (auto* highlighter = markdownLinkHighlighterFor(this)) {
            highlighter->rehighlight();
        }
    }

    if (event && event->type() == QEvent::FontChange
        && property(FormattingRequestedVisibleProperty).toBool()) {
        QTimer::singleShot(0, this, [this] {
            animateFormattingToolbar(this, true);
        });
    }

    if (event && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (escapeRichLinkOnDoubleSpace(this, keyEvent)
            || escapeMarkdownLinkOnDoubleSpace(this, keyEvent)) {
            return true;
        }
    }

    const bool handled = InteractiveTextEdit::event(event);
    syncStructuralPlaceholder(this);
    return handled;
}

} // namespace Mattermost
