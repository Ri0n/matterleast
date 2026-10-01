/**
 * @file MessageTextEditWidget.h
 * @brief Plain-Markdown message composer with formatting helpers.
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of MatterLeast.
 *
 * MatterLeast is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#pragma once

#include <QAction>

#include "widgets/InteractiveTextEdit.h"

class QContextMenuEvent;
class QFocusEvent;
class QMimeData;
class QResizeEvent;

namespace Mattermost {

class MessageTextEditWidget : public InteractiveTextEdit
{
    Q_OBJECT

public:
    explicit MessageTextEditWidget(QWidget* parent = nullptr);
    ~MessageTextEditWidget() override;

    void keyPressEvent(QKeyEvent* event) override;
    bool hasNonEmptyText();

public slots:
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
    void formattingToolbarVisibilityChanged(bool visible);

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void insertFromMimeData(const QMimeData* source) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void updateHeightToContents();
    void wrapMarkdownSelection(const QString& before, const QString& after);
    void prefixMarkdownLines(const QString& prefix, bool numbered = false);
    void setFormattingToolbarVisible(bool visible);
    bool formattingToolbarPreferredVisible() const;
    void normalizeFormattingToolbar();
    void animateFormattingToolbar(bool visible);
    bool editLinkAtCursor();
    bool removeLinkAtCursor();
    bool selectionContainsLink() const;

    bool formattingToolbarVisible_ = false;
};

} // namespace Mattermost
