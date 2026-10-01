#pragma once

#include <algorithm>

#include <QDynamicPropertyChangeEvent>
#include <QEasingCurve>
#include <QEvent>
#include <QKeyEvent>
#include <QPalette>
#include <QPointer>
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTimer>
#include <QVariantAnimation>
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
constexpr char PostEditModeHookProperty[] =
    "_matterleast_post_edit_mode_hook";
constexpr char MarkdownLinkHighlighterObjectName[] =
    "_matterleast_markdown_link_highlighter";
constexpr int FormattingToolbarAnimationMs = 240;

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

inline void animateFormattingToolbar(MessageTextEditWidget* editor, bool visible)
{
    QWidget* toolbar = formattingToolbarFor(editor);
    if (!toolbar) {
        return;
    }

    auto* previous = toolbar->findChild<QVariantAnimation*>(
        QString::fromLatin1(FormattingAnimationObjectName),
        Qt::FindDirectChildrenOnly);
    int interruptedHeight = -1;
    if (previous) {
        interruptedHeight = previous->currentValue().toInt();
        previous->stop();
        previous->deleteLater();
    }

    const int targetHeight = std::max(1, toolbar->sizeHint().height());
    const int startHeight = interruptedHeight >= 0
        ? qBound(0, interruptedHeight, targetHeight)
        : (visible ? 0 : std::max(toolbar->height(), targetHeight));

    // ChatArea.ui owns semantic visibility. Keep the widget painted during the
    // collapsing leg so the layout shrinks instead of disappearing abruptly.
    toolbar->show();
    toolbar->setMaximumHeight(startHeight);

    auto* animation = new QVariantAnimation(toolbar);
    animation->setObjectName(QString::fromLatin1(FormattingAnimationObjectName));
    animation->setDuration(FormattingToolbarAnimationMs);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->setStartValue(startHeight);
    animation->setEndValue(visible ? targetHeight : 0);

    QObject::connect(animation, &QVariantAnimation::valueChanged,
                     toolbar, [toolbar](const QVariant& value) {
        toolbar->setMaximumHeight(std::max(0, value.toInt()));
    });
    QObject::connect(animation, &QVariantAnimation::finished,
                     editor, [editor, toolbar, animation, visible] {
        animation->deleteLater();
        const bool requestedVisible =
            editor->property(FormattingRequestedVisibleProperty).toBool();
        if (requestedVisible != visible) {
            return;
        }

        toolbar->setMaximumHeight(QWIDGETSIZE_MAX);
        if (!visible) {
            toolbar->hide();
        }
    });

    editor->setProperty(FormattingRequestedVisibleProperty, visible);
    animation->start();
}

inline void ensureFormattingAnimationHook(MessageTextEditWidget* editor)
{
    if (!editor || editor->property(FormattingAnimationHookProperty).toBool()) {
        return;
    }

    editor->setProperty(FormattingAnimationHookProperty, true);
    QObject::connect(
        editor,
        &MessageTextEditWidget::formattingToolbarVisibilityChanged,
        editor,
        [editor](bool visible) {
            animateFormattingToolbar(editor, visible);
        });
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

    if (event && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (escapeRichLinkOnDoubleSpace(this, keyEvent)
            || escapeMarkdownLinkOnDoubleSpace(this, keyEvent)) {
            return true;
        }
    }

    return InteractiveTextEdit::event(event);
}

} // namespace Mattermost
