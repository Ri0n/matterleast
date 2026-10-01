/**
 * @file MessageTextEditWidget.cpp
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

#include "MessageTextEditWidget.h"

#include <algorithm>
#include <cmath>

#include <QAbstractTextDocumentLayout>
#include <QDebug>
#include <QFontDatabase>
#include <QFrame>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QPalette>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFormat>
#include <QTextList>
#include <QTextListFormat>
#include <QTimer>

#include "Settings.h"
#include "options/MLOptions.h"

namespace Mattermost {
namespace {

constexpr int ComposerMaximumHeight = 300;

QTextDocument::MarkdownFeatures markdownFeatures()
{
    return QTextDocument::MarkdownDialectGitHub;
}

QString serializedMarkdown(const QTextDocument& document)
{
    QString markdown = document.toMarkdown(markdownFeatures());
    // QTextDocument serializes the final paragraph terminator. Mattermost
    // messages do not require that synthetic newline, while real trailing blank
    // lines remain represented by additional newlines.
    if (markdown.endsWith(QLatin1Char('\n'))) {
        markdown.chop(1);
    }
    return markdown;
}

bool hasPrimaryModifier(Qt::KeyboardModifiers modifiers)
{
#ifdef Q_OS_MACOS
    return modifiers.testFlag(Qt::MetaModifier);
#else
    return modifiers.testFlag(Qt::ControlModifier);
#endif
}

} // namespace

MessageTextEditWidget::MessageTextEditWidget(QWidget* parent)
    : InteractiveTextEdit(parent)
{
    setSubmitOnEnter(true);
    setAcceptRichText(false);

    auto* sendWithCtrlEnter = MLOptions::instance()->optionObject<bool>(
        COMPOSER_SEND_WITH_CTRL_ENTER,
        COMPOSER_SEND_WITH_CTRL_ENTER_DEFAULT);
    setSubmitOnCtrlEnter(sendWithCtrlEnter->value().toBool());
    connect(sendWithCtrlEnter, &MLOptionObject::changed, this,
            [this](const QVariant& value) {
        setSubmitOnCtrlEnter(value.toBool());
    });

    setSubmitHandler([this] { emit enterPressed(); });

    // Keep the composer visually continuous with the action row below it.
    // BackgroundRole references remain palette-driven instead of baking the
    // current theme color into the editor.
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setBackgroundRole(QPalette::Window);
    setAutoFillBackground(true);
    viewport()->setBackgroundRole(QPalette::Window);
    viewport()->setAutoFillBackground(true);
    document()->setDocumentMargin(3.0);

    connect(this, &QTextEdit::textChanged, this, [this] {
        if (isRichTextEditing() && !loadingMarkdown_) {
            richDocumentDirty_ = true;
        }
        updateHeightToContents();
        // The block inserted by Shift+Enter may finish layout after the key
        // event. Measure once more on the next event-loop turn.
        QTimer::singleShot(0, this, &MessageTextEditWidget::updateHeightToContents);
    });
    connect(document(), &QTextDocument::blockCountChanged, this,
            [this](int) { updateHeightToContents(); });
    connect(document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged,
            this, [this](const QSizeF&) { updateHeightToContents(); });

    QTimer::singleShot(0, this, &MessageTextEditWidget::updateHeightToContents);
}

MessageTextEditWidget::~MessageTextEditWidget() = default;

QString MessageTextEditWidget::markdownText() const
{
    if (!isRichTextEditing()) {
        return QTextEdit::toPlainText();
    }
    if (!richDocumentDirty_) {
        return richSourceMarkdown_;
    }
    return serializedMarkdown(*document());
}

void MessageTextEditWidget::setMarkdownText(const QString& markdown)
{
    loadingMarkdown_ = true;
    if (isRichTextEditing()) {
        richSourceMarkdown_ = markdown;
        richDocumentDirty_ = false;
        document()->setMarkdown(markdown, markdownFeatures());
    } else {
        QTextEdit::setPlainText(markdown);
    }
    loadingMarkdown_ = false;
    updateHeightToContents();
}

void MessageTextEditWidget::setRichTextEditing(bool enabled)
{
    const EditingMode next = enabled ? EditingMode::RichText : EditingMode::Markdown;
    if (editingMode_ == next) {
        return;
    }

    const QString markdown = markdownText();
    editingMode_ = next;
    setAcceptRichText(enabled);

    loadingMarkdown_ = true;
    if (enabled) {
        richSourceMarkdown_ = markdown;
        richDocumentDirty_ = false;
        document()->setMarkdown(markdown, markdownFeatures());
    } else {
        QTextEdit::setPlainText(markdown);
        richSourceMarkdown_.clear();
        richDocumentDirty_ = false;
    }
    loadingMarkdown_ = false;

    moveCursor(QTextCursor::End);
    updateHeightToContents();
    emit editingModeChanged(editingMode_);
}

void MessageTextEditWidget::markRichDocumentChanged()
{
    if (isRichTextEditing() && !loadingMarkdown_) {
        richDocumentDirty_ = true;
    }
}

void MessageTextEditWidget::applyRichCharFormat(const QTextCharFormat& format)
{
    QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        cursor.mergeCharFormat(format);
        setTextCursor(cursor);
    } else {
        mergeCurrentCharFormat(format);
    }
    markRichDocumentChanged();
}

void MessageTextEditWidget::wrapMarkdownSelection(const QString& before,
                                                   const QString& after)
{
    QTextCursor cursor = textCursor();
    const int selectionStart = cursor.selectionStart();
    QString selected = cursor.selectedText();
    selected.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));

    if (cursor.hasSelection()) {
        cursor.insertText(before + selected + after);
        cursor.setPosition(selectionStart + before.size());
        cursor.setPosition(selectionStart + before.size() + selected.size(),
                           QTextCursor::KeepAnchor);
    } else {
        cursor.insertText(before + after);
        cursor.setPosition(selectionStart + before.size());
    }
    setTextCursor(cursor);
}

void MessageTextEditWidget::prefixMarkdownLines(const QString& prefix, bool numbered)
{
    QTextCursor cursor = textCursor();
    const QString text = QTextEdit::toPlainText();
    const int selectionStart = cursor.hasSelection()
        ? cursor.selectionStart() : cursor.position();
    const int selectionEnd = cursor.hasSelection()
        ? cursor.selectionEnd() : cursor.position();

    int lineStart = text.lastIndexOf(QLatin1Char('\n'), std::max(0, selectionStart - 1));
    lineStart = lineStart < 0 ? 0 : lineStart + 1;
    int lineEnd = text.indexOf(QLatin1Char('\n'), selectionEnd);
    if (lineEnd < 0) {
        lineEnd = text.size();
    }

    QStringList lines = text.mid(lineStart, lineEnd - lineStart).split(QLatin1Char('\n'));
    for (int index = 0; index < lines.size(); ++index) {
        const QString linePrefix = numbered
            ? QString::number(index + 1) + QStringLiteral(". ") : prefix;
        lines[index].prepend(linePrefix);
    }

    cursor.setPosition(lineStart);
    cursor.setPosition(lineEnd, QTextCursor::KeepAnchor);
    cursor.insertText(lines.join(QLatin1Char('\n')));
    setTextCursor(cursor);
}

void MessageTextEditWidget::toggleBold()
{
    if (!isRichTextEditing()) {
        wrapMarkdownSelection(QStringLiteral("**"), QStringLiteral("**"));
        return;
    }
    QTextCharFormat format;
    const bool bold = currentCharFormat().fontWeight() >= QFont::Bold;
    format.setFontWeight(bold ? QFont::Normal : QFont::Bold);
    applyRichCharFormat(format);
}

void MessageTextEditWidget::toggleItalic()
{
    if (!isRichTextEditing()) {
        wrapMarkdownSelection(QStringLiteral("_"), QStringLiteral("_"));
        return;
    }
    QTextCharFormat format;
    format.setFontItalic(!currentCharFormat().fontItalic());
    applyRichCharFormat(format);
}

void MessageTextEditWidget::toggleStrikeOut()
{
    if (!isRichTextEditing()) {
        wrapMarkdownSelection(QStringLiteral("~~"), QStringLiteral("~~"));
        return;
    }
    QTextCharFormat format;
    format.setFontStrikeOut(!currentCharFormat().fontStrikeOut());
    applyRichCharFormat(format);
}

void MessageTextEditWidget::toggleInlineCode()
{
    if (!isRichTextEditing()) {
        wrapMarkdownSelection(QStringLiteral("`"), QStringLiteral("`"));
        return;
    }
    QTextCharFormat format;
    const bool fixed = currentCharFormat().fontFixedPitch();
    format.setFontFixedPitch(!fixed);
    if (!fixed) {
        format.setFontFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    }
    applyRichCharFormat(format);
}

void MessageTextEditWidget::toggleCodeBlock()
{
    if (!isRichTextEditing()) {
        wrapMarkdownSelection(QStringLiteral("```\n"), QStringLiteral("\n```"));
        return;
    }

    QTextCursor cursor = textCursor();
    QTextBlockFormat blockFormat = cursor.blockFormat();
    const bool code = blockFormat.hasProperty(QTextFormat::BlockCodeFence);
    if (code) {
        blockFormat.clearProperty(QTextFormat::BlockCodeFence);
        blockFormat.clearProperty(QTextFormat::BlockCodeLanguage);
    } else {
        blockFormat.setProperty(QTextFormat::BlockCodeFence, QStringLiteral("```"));
    }
    cursor.mergeBlockFormat(blockFormat);

    QTextCharFormat charFormat;
    charFormat.setFontFixedPitch(!code);
    if (!code) {
        charFormat.setFontFamily(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    }
    cursor.mergeCharFormat(charFormat);
    setTextCursor(cursor);
    markRichDocumentChanged();
}

void MessageTextEditWidget::toggleQuote()
{
    if (!isRichTextEditing()) {
        prefixMarkdownLines(QStringLiteral("> "));
        return;
    }

    QTextCursor cursor = textCursor();
    QTextBlockFormat format = cursor.blockFormat();
    const int quoteLevel = format.intProperty(QTextFormat::BlockQuoteLevel);
    if (quoteLevel > 0) {
        format.clearProperty(QTextFormat::BlockQuoteLevel);
    } else {
        format.setProperty(QTextFormat::BlockQuoteLevel, 1);
    }
    cursor.mergeBlockFormat(format);
    setTextCursor(cursor);
    markRichDocumentChanged();
}

void MessageTextEditWidget::toggleBulletList()
{
    if (!isRichTextEditing()) {
        prefixMarkdownLines(QStringLiteral("- "));
        return;
    }

    QTextCursor cursor = textCursor();
    QTextListFormat format;
    format.setStyle(QTextListFormat::ListDisc);
    cursor.createList(format);
    setTextCursor(cursor);
    markRichDocumentChanged();
}

void MessageTextEditWidget::toggleNumberedList()
{
    if (!isRichTextEditing()) {
        prefixMarkdownLines(QString(), true);
        return;
    }

    QTextCursor cursor = textCursor();
    QTextListFormat format;
    format.setStyle(QTextListFormat::ListDecimal);
    cursor.createList(format);
    setTextCursor(cursor);
    markRichDocumentChanged();
}

void MessageTextEditWidget::insertLink()
{
    bool accepted = false;
    const QString url = QInputDialog::getText(
        this, tr("Insert link"), tr("URL:"), QLineEdit::Normal,
        QString(), &accepted).trimmed();
    if (!accepted || url.isEmpty()) {
        return;
    }

    QTextCursor cursor = textCursor();
    QString label = cursor.selectedText();
    label.replace(QChar::ParagraphSeparator, QLatin1Char(' '));
    if (label.isEmpty()) {
        label = url;
    }

    if (!isRichTextEditing()) {
        cursor.insertText(QStringLiteral("[%1](%2)").arg(label, url));
        setTextCursor(cursor);
        return;
    }

    QTextCharFormat format;
    format.setAnchor(true);
    format.setAnchorHref(url);
    format.setFontUnderline(true);
    if (cursor.hasSelection()) {
        cursor.mergeCharFormat(format);
    } else {
        cursor.insertText(label, format);
    }
    setTextCursor(cursor);
    markRichDocumentChanged();
}

void MessageTextEditWidget::keyPressEvent(QKeyEvent* event)
{
    if (!event) {
        return;
    }

    // Completion navigation owns its keys while visible. Formatting shortcuts
    // remain editor-level only when no completion popup is intercepting input.
    if (!completionPopupVisible()) {
        if (event->matches(QKeySequence::Bold)) {
            toggleBold();
            event->accept();
            return;
        }
        if (event->matches(QKeySequence::Italic)) {
            toggleItalic();
            event->accept();
            return;
        }

        const Qt::KeyboardModifiers modifiers = event->modifiers();
        if (hasPrimaryModifier(modifiers)
            && event->key() == Qt::Key_K
            && !modifiers.testFlag(Qt::AltModifier)) {
            insertLink();
            event->accept();
            return;
        }
        if (hasPrimaryModifier(modifiers)
            && modifiers.testFlag(Qt::ShiftModifier)
            && event->key() == Qt::Key_X) {
            toggleStrikeOut();
            event->accept();
            return;
        }

        switch (event->key()) {
        case Qt::Key_Up:
            emit upArrowPressed();
            break;
        case Qt::Key_Escape:
            emit escapePressed();
            break;
        default:
            break;
        }
    }

    InteractiveTextEdit::keyPressEvent(event);
}

void MessageTextEditWidget::resizeEvent(QResizeEvent* event)
{
    QTextEdit::resizeEvent(event);
    updateHeightToContents();
}

void MessageTextEditWidget::updateHeightToContents()
{
    if (!document() || !document()->documentLayout()) {
        return;
    }

    const QMargins margins = contentsMargins();
    const int chromeHeight = margins.top() + margins.bottom() + 2 * frameWidth();
    const int documentMargins = static_cast<int>(std::ceil(document()->documentMargin() * 2.0));
    const int lineHeight = fontMetrics().lineSpacing();
    const int oneLineHeight = lineHeight + documentMargins + chromeHeight;
    const int laidOutHeight = static_cast<int>(std::ceil(
        document()->documentLayout()->documentSize().height())) + chromeHeight;
    const int explicitLineHeight = std::max(1, document()->blockCount()) * lineHeight
        + documentMargins + chromeHeight;
    const int wantedHeight = std::clamp(
        std::max({oneLineHeight, laidOutHeight, explicitLineHeight}),
        oneLineHeight, ComposerMaximumHeight);

    if (height() != wantedHeight) {
        // Do not derive the next height limit from maximumHeight():
        // setFixedHeight() deliberately changes maximumHeight() to the current
        // value, which would otherwise permanently cap the editor at its first
        // one-line measurement.
        setFixedHeight(wantedHeight);
        updateGeometry();
    }
}

bool MessageTextEditWidget::hasNonEmptyText()
{
    return document()->characterCount() > 1;
}

} /* namespace Mattermost */
