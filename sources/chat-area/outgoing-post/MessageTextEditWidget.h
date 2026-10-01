/**
 * @file MessageTextEditWidget.h
 * @brief
 * @author Lyubomir Filipov
 * @date Jan 7, 2022
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of MatterLeast.
 *
 * MatterLeast is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MatterLeast is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with MatterLeast. if not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <QAction>
#include <QPainter>

#include "chat-area/RichTextBlockPresentation.h"
#include "widgets/InteractiveTextEdit.h"

class QContextMenuEvent;
class QEvent;
class QFocusEvent;
class QMimeData;
class QPaintEvent;
class QResizeEvent;
class QTextCharFormat;

namespace Mattermost {

class MessageTextEditWidget: public InteractiveTextEdit {
    Q_OBJECT
    Q_PROPERTY(bool richTextEditing READ isRichTextEditing WRITE setRichTextEditing)
public:
    enum class EditingMode {
        Markdown,
        RichText,
    };
    Q_ENUM(EditingMode)

    MessageTextEditWidget(QWidget *parent = nullptr);
    ~MessageTextEditWidget() override;

    void keyPressEvent(QKeyEvent* event) override;
    bool hasNonEmptyText();

    EditingMode editingMode() const { return editingMode_; }
    bool isRichTextEditing() const { return editingMode_ == EditingMode::RichText; }

    /** Canonical Mattermost message representation regardless of editor mode. */
    QString markdownText() const;
    void setMarkdownText(const QString& markdown);

    /**
     * Preserve the historical QTextEdit-facing API for callers such as
     * OutgoingPostCreator, but make it Markdown-aware in rich mode.
     */
    QString toPlainText() const { return markdownText(); }
    void setPlainText(const QString& text) { setMarkdownText(text); }

public slots:
    void setRichTextEditing(bool enabled);
    void setFormattingToolbarPreferredVisible(bool visible);
    void toggleBold();
    void toggleItalic();
    void toggleStrikeOut();
    void toggleInlineCode();
    void toggleCodeBlock();
    void toggleQuote();
    void toggleBulletList();
    void toggleNumberedList();
    void insertLink();

signals:
    void enterPressed();
    void escapePressed();
    void upArrowPressed();
    void editingModeChanged(Mattermost::MessageTextEditWidget::EditingMode mode);
    void formattingToolbarVisibilityChanged(bool visible);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override
    {
        InteractiveTextEdit::paintEvent(event);
        if (isRichTextEditing()) {
            // QTextEdit explicitly requires custom paint to target viewport().
            // Draw after the base implementation so structural chrome cannot be
            // overwritten by the document/background paint pass.
            QPainter painter(viewport());
            paintComposerRichTextBlockDecorations(*this, painter);
        }
    }
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void insertFromMimeData(const QMimeData* source) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateHeightToContents();
    void wrapMarkdownSelection(const QString& before,
                               const QString& after);
    void prefixMarkdownLines(const QString& prefix, bool numbered = false);
    void applyRichCharFormat(const QTextCharFormat& format);
    void markRichDocumentChanged();
    void setFormattingToolbarVisible(bool visible);
    bool formattingToolbarPreferredVisible() const;
    bool editLinkAtCursor();
    bool removeLinkAtCursor();
    bool selectionContainsLink() const;

    EditingMode editingMode_ = EditingMode::Markdown;
    QString richSourceMarkdown_;
    bool richDocumentDirty_ = false;
    bool loadingMarkdown_ = false;
    bool formattingToolbarVisible_ = false;
    bool postEditModeForced_ = false;
    bool restoreRichAfterPostEdit_ = false;
};

} /* namespace Mattermost */

#include "MessageTextEditWidgetInteraction.inl"
