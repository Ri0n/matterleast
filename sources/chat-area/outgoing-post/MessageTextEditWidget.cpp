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
#include "RichTextEditorCommands.h"

#include <algorithm>
#include <cmath>

#include <QAbstractTextDocumentLayout>
#include <QContextMenuEvent>
#include <QDebug>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFrame>
#include <QInputDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPalette>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QStringList>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextFragment>
#include <QTimer>
#include <QUrl>

#include "Settings.h"
#include "options/MLOptions.h"

namespace Mattermost {
namespace {

constexpr int ComposerMaximumHeight = 300;

struct MarkdownLinkMatch {
    int start = -1;
    int end = -1;
    QString label;
    QString url;

    bool isValid() const { return start >= 0 && end > start; }
};

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

void setFontFamilyCompat(QTextCharFormat& format, const QString& family)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    format.setFontFamilies(QStringList {family});
#else
    format.setFontFamily(family);
#endif
}

QString normalizedHttpUrl(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()
        || QRegularExpression(QStringLiteral("\\s")).match(trimmed).hasMatch()) {
        return {};
    }

    const QUrl url(trimmed, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QStringLiteral("http")
                           && scheme != QStringLiteral("https"))) {
        return {};
    }
    return url.toString(QUrl::FullyEncoded);
}

QString httpUrlFromMimeData(const QMimeData* source)
{
    if (!source) {
        return {};
    }

    if (source->hasUrls()) {
        const QList<QUrl> urls = source->urls();
        if (urls.size() == 1 && !urls.first().isLocalFile()) {
            const QString url = normalizedHttpUrl(urls.first().toString());
            if (!url.isEmpty()) {
                return url;
            }
        }
    }
    if (source->hasText()) {
        return normalizedHttpUrl(source->text());
    }
    return {};
}

QString escapedMarkdownLinkLabel(QString label)
{
    label.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    label.replace(QLatin1Char('['), QStringLiteral("\\["));
    label.replace(QLatin1Char(']'), QStringLiteral("\\]"));
    return label;
}

MarkdownLinkMatch markdownLinkAt(const QString& text,
                                 int selectionStart,
                                 int selectionEnd)
{
    static const QRegularExpression expression(
        QStringLiteral("\\[([^\\]\\n]+)\\]\\((https?://[^)\\s]+)\\)"),
        QRegularExpression::CaseInsensitiveOption);

    auto it = expression.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const int start = match.capturedStart(0);
        const int end = match.capturedEnd(0);
        const bool intersects = selectionStart == selectionEnd
            ? selectionStart >= start && selectionStart <= end
            : selectionStart < end && selectionEnd > start;
        if (!intersects) {
            continue;
        }

        MarkdownLinkMatch result;
        result.start = start;
        result.end = end;
        result.label = match.captured(1);
        result.url = match.captured(2);
        return result;
    }
    return {};
}

bool richLinkAt(QTextDocument* document,
                const QTextCursor& current,
                QTextCursor* range,
                QString* url)
{
    if (!document) {
        return false;
    }

    const int selectionStart = current.hasSelection()
        ? current.selectionStart() : current.position();
    const int selectionEnd = current.hasSelection()
        ? current.selectionEnd() : current.position();
    QTextBlock block = document->findBlock(
        qBound(0, selectionStart, std::max(0, document->characterCount() - 1)));

    for (; block.isValid() && block.position() <= selectionEnd;
         block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (!fragment.isValid() || !fragment.charFormat().isAnchor()) {
                continue;
            }
            const int start = fragment.position();
            const int end = start + fragment.length();
            const bool intersects = selectionStart == selectionEnd
                ? selectionStart >= start && selectionStart <= end
                : selectionStart < end && selectionEnd > start;
            if (!intersects) {
                continue;
            }

            if (range) {
                QTextCursor cursor(document);
                cursor.setPosition(start);
                cursor.setPosition(end, QTextCursor::KeepAnchor);
                *range = cursor;
            }
            if (url) {
                *url = fragment.charFormat().anchorHref();
            }
            return true;
        }
    }
    return false;
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

    auto* toolbarVisible = MLOptions::instance()->optionObject<bool>(
        COMPOSER_FORMATTING_TOOLBAR_VISIBLE,
        COMPOSER_FORMATTING_TOOLBAR_VISIBLE_DEFAULT);
    connect(toolbarVisible, &MLOptionObject::changed, this,
            [this](const QVariant& value) {
        if (hasFocus()) {
            setFormattingToolbarVisible(value.toBool());
        }
    });

    setSubmitHandler([this] { emit enterPressed(); });

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

bool MessageTextEditWidget::formattingToolbarPreferredVisible() const
{
    return MLOptions::instance()->optionObject<bool>(
        COMPOSER_FORMATTING_TOOLBAR_VISIBLE,
        COMPOSER_FORMATTING_TOOLBAR_VISIBLE_DEFAULT)->value().toBool();
}

void MessageTextEditWidget::setFormattingToolbarVisible(bool visible)
{
    if (formattingToolbarVisible_ == visible) {
        return;
    }
    formattingToolbarVisible_ = visible;
    emit formattingToolbarVisibilityChanged(visible);
}

void MessageTextEditWidget::setFormattingToolbarPreferredVisible(bool visible)
{
    MLOptions::instance()->optionObject<bool>(
        COMPOSER_FORMATTING_TOOLBAR_VISIBLE,
        COMPOSER_FORMATTING_TOOLBAR_VISIBLE_DEFAULT)->setValue(visible);
    setFormattingToolbarVisible(hasFocus() && visible);
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
        setFontFamilyCompat(
            format,
            QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
    }
    applyRichCharFormat(format);
}

void MessageTextEditWidget::toggleCodeBlock()
{
    if (!isRichTextEditing()) {
        wrapMarkdownSelection(QStringLiteral("```\n"), QStringLiteral("\n```"));
        return;
    }

    if (RichTextEditorCommands::toggleCodeBlock(*this)) {
        markRichDocumentChanged();
    }
}

void MessageTextEditWidget::toggleQuote()
{
    if (!isRichTextEditing()) {
        prefixMarkdownLines(QStringLiteral("> "));
        return;
    }

    if (RichTextEditorCommands::toggleQuote(*this)) {
        markRichDocumentChanged();
    }
}

void MessageTextEditWidget::toggleBulletList()
{
    if (!isRichTextEditing()) {
        prefixMarkdownLines(QStringLiteral("- "));
        return;
    }

    if (RichTextEditorCommands::toggleList(
            *this, RichTextEditorCommands::ListStyle::Bullet)) {
        markRichDocumentChanged();
    }
}

void MessageTextEditWidget::toggleNumberedList()
{
    if (!isRichTextEditing()) {
        prefixMarkdownLines(QString(), true);
        return;
    }

    if (RichTextEditorCommands::toggleList(
            *this, RichTextEditorCommands::ListStyle::Numbered)) {
        markRichDocumentChanged();
    }
}

bool MessageTextEditWidget::editLinkAtCursor()
{
    if (isRichTextEditing()) {
        QTextCursor range;
        QString currentUrl;
        if (!richLinkAt(document(), textCursor(), &range, &currentUrl)) {
            return false;
        }

        bool accepted = false;
        const QString url = QInputDialog::getText(
            this, tr("Edit link"), tr("URL:"), QLineEdit::Normal,
            currentUrl, &accepted).trimmed();
        if (!accepted || url.isEmpty()) {
            return true;
        }

        QTextCharFormat format;
        format.setAnchor(true);
        format.setAnchorHref(url);
        format.setFontUnderline(true);
        range.mergeCharFormat(format);
        setTextCursor(range);
        markRichDocumentChanged();
        return true;
    }

    const QTextCursor current = textCursor();
    const MarkdownLinkMatch link = markdownLinkAt(
        QTextEdit::toPlainText(), current.selectionStart(), current.selectionEnd());
    if (!link.isValid()) {
        return false;
    }

    bool accepted = false;
    const QString url = QInputDialog::getText(
        this, tr("Edit link"), tr("URL:"), QLineEdit::Normal,
        link.url, &accepted).trimmed();
    if (!accepted || url.isEmpty()) {
        return true;
    }

    QTextCursor replacement(document());
    replacement.setPosition(link.start);
    replacement.setPosition(link.end, QTextCursor::KeepAnchor);
    replacement.insertText(QStringLiteral("[%1](%2)").arg(link.label, url));
    setTextCursor(replacement);
    return true;
}

bool MessageTextEditWidget::removeLinkAtCursor()
{
    if (isRichTextEditing()) {
        QTextCursor range;
        if (!richLinkAt(document(), textCursor(), &range, nullptr)) {
            return false;
        }
        QTextCharFormat format;
        format.setAnchor(false);
        format.setAnchorHref(QString());
        format.setFontUnderline(false);
        range.mergeCharFormat(format);
        setTextCursor(range);
        markRichDocumentChanged();
        return true;
    }

    const QTextCursor current = textCursor();
    const MarkdownLinkMatch link = markdownLinkAt(
        QTextEdit::toPlainText(), current.selectionStart(), current.selectionEnd());
    if (!link.isValid()) {
        return false;
    }
    QTextCursor replacement(document());
    replacement.setPosition(link.start);
    replacement.setPosition(link.end, QTextCursor::KeepAnchor);
    replacement.insertText(link.label);
    setTextCursor(replacement);
    return true;
}

bool MessageTextEditWidget::selectionContainsLink() const
{
    const QTextCursor current = textCursor();
    if (!current.hasSelection()) {
        return false;
    }
    if (isRichTextEditing()) {
        return richLinkAt(document(), current, nullptr, nullptr);
    }
    return markdownLinkAt(
        QTextEdit::toPlainText(), current.selectionStart(), current.selectionEnd())
        .isValid();
}

void MessageTextEditWidget::insertLink()
{
    if (editLinkAtCursor()) {
        return;
    }

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
        cursor.insertText(QStringLiteral("[%1](%2)")
                              .arg(escapedMarkdownLinkLabel(label), url));
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

void MessageTextEditWidget::insertFromMimeData(const QMimeData* source)
{
    QTextCursor cursor = textCursor();
    const QString url = httpUrlFromMimeData(source);
    if (!url.isEmpty() && cursor.hasSelection() && !selectionContainsLink()) {
        QString label = cursor.selectedText();
        label.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
        if (!label.contains(QLatin1Char('\n'))) {
            if (isRichTextEditing()) {
                QTextCharFormat format;
                format.setAnchor(true);
                format.setAnchorHref(url);
                format.setFontUnderline(true);
                cursor.mergeCharFormat(format);
                setTextCursor(cursor);
                markRichDocumentChanged();
            } else {
                cursor.insertText(QStringLiteral("[%1](%2)")
                                      .arg(escapedMarkdownLinkLabel(label), url));
                setTextCursor(cursor);
            }
            return;
        }
    }

    if (source) {
        InteractiveTextEdit::insertFromMimeData(source);
    }
}

void MessageTextEditWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (!event) {
        return;
    }

    QMenu* menu = createStandardContextMenu();
    if (!menu) {
        return;
    }

    bool hasLink = false;
    if (isRichTextEditing()) {
        hasLink = richLinkAt(document(), textCursor(), nullptr, nullptr);
    } else {
        const QTextCursor current = textCursor();
        hasLink = markdownLinkAt(
            QTextEdit::toPlainText(), current.selectionStart(), current.selectionEnd())
            .isValid();
    }

    if (hasLink) {
        menu->addSeparator();
        QAction* editLinkAction = menu->addAction(tr("Edit link…"));
        connect(editLinkAction, &QAction::triggered,
                this, [this] { editLinkAtCursor(); });
        QAction* removeLinkAction = menu->addAction(tr("Remove link"));
        connect(removeLinkAction, &QAction::triggered,
                this, [this] { removeLinkAtCursor(); });
    }

    menu->addSeparator();
    QAction* toolbarAction = menu->addAction(
        formattingToolbarVisible_
            ? tr("Hide formatting toolbar")
            : tr("Show formatting toolbar"));
    connect(toolbarAction, &QAction::triggered, this, [this] {
        setFormattingToolbarPreferredVisible(!formattingToolbarVisible_);
    });

    QAction* modeAction = menu->addAction(
        isRichTextEditing() ? tr("Edit Markdown source")
                            : tr("Use rich text editor"));
    connect(modeAction, &QAction::triggered, this, [this] {
        setRichTextEditing(!isRichTextEditing());
    });

    menu->exec(event->globalPos());
    delete menu;
}

void MessageTextEditWidget::focusInEvent(QFocusEvent* event)
{
    InteractiveTextEdit::focusInEvent(event);
    setFormattingToolbarVisible(formattingToolbarPreferredVisible());
}

void MessageTextEditWidget::focusOutEvent(QFocusEvent* event)
{
    setFormattingToolbarVisible(false);
    InteractiveTextEdit::focusOutEvent(event);
}

void MessageTextEditWidget::keyPressEvent(QKeyEvent* event)
{
    if (!event) {
        return;
    }

    const Qt::KeyboardModifiers modifiers = event->modifiers();
    const bool primaryOnly = hasPrimaryModifier(modifiers)
        && !modifiers.testFlag(Qt::ShiftModifier)
        && !modifiers.testFlag(Qt::AltModifier);
    if (primaryOnly && event->key() == Qt::Key_Up) {
        setFormattingToolbarPreferredVisible(true);
        event->accept();
        return;
    }
    if (primaryOnly && event->key() == Qt::Key_Down) {
        setFormattingToolbarPreferredVisible(false);
        event->accept();
        return;
    }

    // Completion navigation owns its keys while visible. Formatting and
    // structural commands run only when no completion overlay intercepts input.
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

        // Structural rich-text editing gets the key before InteractiveTextEdit
        // applies submit-on-Enter. List/code/quote commands deliberately decline
        // Ctrl+Enter, so the configured explicit-send chord still reaches the
        // global composer policy.
        if (isRichTextEditing()
            && RichTextEditorCommands::handleStructuralKey(*this, *event)) {
            markRichDocumentChanged();
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
        setFixedHeight(wantedHeight);
        updateGeometry();
    }
}

bool MessageTextEditWidget::hasNonEmptyText()
{
    return document()->characterCount() > 1;
}

} /* namespace Mattermost */
