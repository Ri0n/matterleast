/**
 * Copyright 2026 Sergei Ilinykh
 *
 * Interaction-level behavior for MessageTextEditWidget that is intentionally
 * kept separate from Markdown formatting/serialization logic.
 */

#include "MessageTextEditWidget.h"

#include <algorithm>

#include <QDynamicPropertyChangeEvent>
#include <QEasingCurve>
#include <QKeyEvent>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVariantAnimation>
#include <QWidget>

namespace Mattermost {
namespace {

constexpr char EditingPostProperty[] = "_mmqt_editing_post";
constexpr char FormattingAnimationHookProperty[] =
    "_matterleast_formatting_animation_hook";
constexpr char FormattingAnimationObjectName[] =
    "_matterleast_formatting_toolbar_animation";
constexpr int FormattingToolbarAnimationMs = 240;

QWidget* formattingToolbarFor(MessageTextEditWidget* editor)
{
    if (!editor || !editor->parentWidget()) {
        return nullptr;
    }
    return editor->parentWidget()->findChild<QWidget*>(
        QStringLiteral("formattingToolbar"));
}

void animateFormattingToolbar(MessageTextEditWidget* editor, bool visible)
{
    QWidget* toolbar = formattingToolbarFor(editor);
    if (!toolbar) {
        return;
    }

    auto* previous = toolbar->findChild<QVariantAnimation*>(
        QStringLiteral(FormattingAnimationObjectName),
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

    // ChatArea.ui still owns the semantic visibility connection. For hiding it
    // has already called setVisible(false) before this later connection runs;
    // showing it again synchronously lets us animate the collapse without a
    // painted intermediate frame.
    toolbar->show();
    toolbar->setMaximumHeight(startHeight);

    auto* animation = new QVariantAnimation(toolbar);
    animation->setObjectName(QStringLiteral(FormattingAnimationObjectName));
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
            editor->property("_matterleast_formatting_requested_visible")
                .toBool();
        if (requestedVisible != visible) {
            return;
        }

        toolbar->setMaximumHeight(QWIDGETSIZE_MAX);
        if (!visible) {
            toolbar->hide();
        }
    });

    editor->setProperty("_matterleast_formatting_requested_visible", visible);
    animation->start();
}

void ensureFormattingAnimationHook(MessageTextEditWidget* editor)
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

bool escapeRichLinkOnDoubleSpace(MessageTextEditWidget* editor,
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

    // The first space inherited the link format. Replace it with exactly one
    // ordinary space and consume the second keypress, leaving the cursor ready
    // for unlinked text.
    previousCharacter.removeSelectedText();
    previousCharacter.insertText(QStringLiteral(" "), plainFormat);
    editor->setTextCursor(previousCharacter);
    editor->setCurrentCharFormat(plainFormat);
    keyEvent->accept();
    return true;
}

} // namespace

bool MessageTextEditWidget::event(QEvent* event)
{
    // OutgoingPostCreator historically disabled the context menu. Restore the
    // ordinary QTextEdit policy after construction without making that class
    // responsible for rich-editor details.
    if (contextMenuPolicy() != Qt::DefaultContextMenu) {
        setContextMenuPolicy(Qt::DefaultContextMenu);
    }

    ensureFormattingAnimationHook(this);

    if (event && event->type() == QEvent::DynamicPropertyChange) {
        auto* propertyEvent = static_cast<QDynamicPropertyChangeEvent*>(event);
        if (propertyEvent->propertyName() == EditingPostProperty) {
            const bool editing = property(EditingPostProperty).toBool();
            if (editing && !postEditModeForced_) {
                postEditModeForced_ = true;
                restoreRichAfterPostEdit_ = isRichTextEditing();
                if (restoreRichAfterPostEdit_) {
                    // Editing an existing post must expose the exact Mattermost
                    // source. richSourceMarkdown_ still contains the unmodified
                    // original, including code fences and unsupported syntax.
                    setRichTextEditing(false);
                }
            } else if (!editing && postEditModeForced_) {
                const bool restoreRich = restoreRichAfterPostEdit_;
                postEditModeForced_ = false;
                restoreRichAfterPostEdit_ = false;
                if (restoreRich) {
                    setRichTextEditing(true);
                }
            }
        }
    }

    if (event && event->type() == QEvent::KeyPress
        && escapeRichLinkOnDoubleSpace(
            this, static_cast<QKeyEvent*>(event))) {
        return true;
    }

    return InteractiveTextEdit::event(event);
}

} // namespace Mattermost
