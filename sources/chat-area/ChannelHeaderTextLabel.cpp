/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "ChannelHeaderTextLabel.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QAbstractAnimation>
#include <QAbstractTextDocumentLayout>
#include <QDesktopServices>
#include <QEasingCurve>
#include <QEvent>
#include <QPalette>
#include <QPropertyAnimation>
#include <QScrollArea>
#include <QSet>
#include <QSizePolicy>
#include <QTextBlock>
#include <QTextBoundaryFinder>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include "ChatArea.h"
#include "backend/emoji/EmojiInfo.h"
#include "backend/emoji/EmojiRegistryNotifier.h"
#include "navigation/AppNavigationService.h"
#include "post/MessageContentWidget.h"
#include "post/MessageFormatter.h"
#include "ui/EmojiPresentation.h"
#include "ui/PresenceAvatarLabel.h"

namespace Mattermost {
namespace {

constexpr int PopoverAnimationDurationMs = 180;
constexpr int PopoverHorizontalMargin = 16;
constexpr int PopoverBottomMargin = 8;

const QSet<QString>& unicodeEmojiStrings()
{
    static const QSet<QString> emojiStrings = [] {
        QSet<QString> result;

        for (int category = 0; category < EmojiCategory::COUNT; ++category) {
            if (category == EmojiCategory::custom) {
                continue;
            }

            const int skinToneCount = category == EmojiCategory::people
                ? EmojiSkinTone::COUNT
                : 1;
            for (int skinTone = 0; skinTone < skinToneCount; ++skinTone) {
                const QVector<Emoji> emojis = EmojiInfo::getAllEmojis(category, skinTone);
                for (const Emoji& emoji : emojis) {
                    const QString glyph = emoji.unicodeString.trimmed();
                    if (!glyph.isEmpty() && !glyph.contains(QStringLiteral("<img"))) {
                        result.insert(glyph);
                    }
                }
            }
        }

        return result;
    }();

    return emojiStrings;
}

QString formatCollapsedTopic(const QString& text, const QFont& font)
{
    const QString html = EmojiPresentation::normalizeHtml(
        MessageFormatter::formatMessageText(text),
        font,
        EmojiPresentation::Mode::Inline);

    QTextDocument document;
    document.setDefaultFont(font);
    document.setDocumentMargin(0);
    document.setHtml(html);

    const QString plainText = document.toPlainText();
    if (!plainText.isEmpty()) {
        const qreal scale = EmojiPresentation::fontScale(EmojiPresentation::Mode::Inline);
        const QSet<QString>& emojiStrings = unicodeEmojiStrings();
        QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, plainText);
        finder.toStart();

        int start = 0;
        while (true) {
            const int end = finder.toNextBoundary();
            if (end < 0) {
                break;
            }

            const QString grapheme = plainText.mid(start, end - start);
            if (emojiStrings.contains(grapheme)) {
                QTextCursor cursor(&document);
                cursor.setPosition(start);

                qreal pointSize = cursor.charFormat().fontPointSize();
                if (pointSize <= 0.0) {
                    pointSize = document.defaultFont().pointSizeF();
                }

                if (pointSize > 0.0) {
                    cursor.setPosition(end, QTextCursor::KeepAnchor);
                    QTextCharFormat emojiFormat;
                    emojiFormat.setFontPointSize(pointSize * scale);
                    cursor.mergeCharFormat(emojiFormat);
                }
            }

            start = end;
        }
    }

    return document.toHtml();
}

} // namespace

ChannelHeaderTextLabel::ChannelHeaderTextLabel(QWidget* parent)
    : QLabel(parent)
{
    setTextFormat(Qt::RichText);
    setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
    setOpenExternalLinks(false);
    setWordWrap(false);
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    installEventFilter(this);

    connect(this, &QLabel::linkActivated, this, [this](const QString& href) {
        openLink(QUrl(href));
    });
    connect(&EmojiRegistryNotifier::instance(),
            &EmojiRegistryNotifier::customEmojiAdded,
            this,
            [this](const QString& name) {
        const QString token = QLatin1Char(':') + name + QLatin1Char(':');
        if (!sourceText.contains(token)) {
            return;
        }
        // Topics are often rendered before the asynchronous custom-emoji
        // download finishes. Reformat the original source once the referenced
        // emoji enters EmojiInfo rather than leaving the literal :name: text.
        setText(sourceText);
    });

    hideTimer.setSingleShot(true);
    hideTimer.setInterval(120);
    connect(&hideTimer, &QTimer::timeout, this, &ChannelHeaderTextLabel::hidePopover);

    updateCollapsedHeight();
}

void ChannelHeaderTextLabel::setText(const QString& text)
{
    // A direct-message presence is presentation state, not channel header text.
    // Route it to the avatar badge so we use the same AvatarUtils visual as the
    // timeline and never depend on rich-text foreground palette propagation.
    if (presenceRoutingEnabled && PresenceAvatarLabel::isPresenceStatus(text)) {
        sourceText.clear();
        formattedText.clear();
        popoverSourceText.clear();
        QLabel::clear();
        hidePopoverImmediately();
        if (popoverContent) {
            popoverContent->clear();
            popoverContent->setMinimumHeight(0);
        }
        if (QWidget* host = parentWidget()) {
            if (auto* avatar = host->findChild<PresenceAvatarLabel*>(
                    QStringLiteral("userAvatar"))) {
                avatar->setStatus(text);
            }
        }
        hide();
        return;
    }

    sourceText = text;

    // QTextDocument::toHtml() returns a complete HTML document even for an
    // empty Markdown source. Keep QLabel::text() genuinely empty here because
    // ChatArea uses text().isEmpty() to decide whether a DM presence value still
    // needs to be installed.
    if (text.isEmpty()) {
        formattedText.clear();
        popoverSourceText.clear();
        QLabel::setText(QString());
        updateCollapsedHeight();
        hidePopoverImmediately();
        if (popoverContent) {
            popoverContent->clear();
            popoverContent->setMinimumHeight(0);
        }
        hide();
        return;
    }

    show();
    formattedText = formatCollapsedTopic(text, font());
    QLabel::setText(formattedText);
    updateCollapsedHeight();

    if (popoverContent && popoverSourceText != sourceText) {
        popoverContent->setMinimumHeight(0);
        popoverContent->setMessage(sourceText);
        popoverSourceText = sourceText;
        configurePopoverLinks();
    }

    if (popover && popover->isVisible()) {
        if (isOverflowing()) {
            QTimer::singleShot(0, this, [this] {
                if (popover && popover->isVisible()) {
                    updatePopoverGeometry(true);
                }
            });
        } else {
            hidePopover();
        }
    }
}

void ChannelHeaderTextLabel::setLinkHandler(LinkHandler handler)
{
    linkHandler = std::move(handler);
}

QSize ChannelHeaderTextLabel::sizeHint() const
{
    return QSize(0, std::max(1, fontMetrics().lineSpacing() + 6));
}

QSize ChannelHeaderTextLabel::minimumSizeHint() const
{
    return QSize(0, std::max(1, fontMetrics().lineSpacing() + 6));
}

void ChannelHeaderTextLabel::updateCollapsedHeight()
{
    const int height = std::max(1, fontMetrics().lineSpacing() + 6);
    setMinimumHeight(height);
    setMaximumHeight(height);
}

bool ChannelHeaderTextLabel::isOverflowing() const
{
    if (sourceText.isEmpty() || width() <= 0) {
        return false;
    }

    if (sourceText.contains(QLatin1Char('\n'))) {
        return true;
    }

    QTextDocument document;
    document.setDefaultFont(font());
    document.setDocumentMargin(0);
    document.setHtml(formattedText);

    if (document.blockCount() > 1) {
        return true;
    }

    return std::ceil(document.idealWidth()) > std::max(1, width() - 4);
}

void ChannelHeaderTextLabel::ensurePopover()
{
    QWidget* host = nullptr;
    for (QWidget* candidate = this; candidate; candidate = candidate->parentWidget()) {
        if (qobject_cast<ChatArea*>(candidate)) {
            host = candidate;
            break;
        }
    }
    if (!host) {
        host = window();
    }
    if (!host) {
        return;
    }

    if (popover && popover->parentWidget() == host) {
        return;
    }

    if (popoverAnimation) {
        popoverAnimation->stop();
    }
    if (popover) {
        popover->deleteLater();
    }
    popoverSourceText.clear();
    popoverContent.clear();
    popoverContainer.clear();
    popoverAnimation.clear();

    auto* scrollArea = new QScrollArea(host);
    scrollArea->setObjectName(QStringLiteral("channelHeaderTextPopover"));
    scrollArea->setFrameShape(QFrame::NoFrame);
    scrollArea->setAutoFillBackground(true);
    scrollArea->setWidgetResizable(true);
    scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scrollArea->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    auto* container = new QWidget;
    container->setObjectName(QStringLiteral("channelHeaderTextPopoverContainer"));
    auto* layout = new QVBoxLayout(container);
    layout->setSpacing(0);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* content = new MessageContentWidget;
    content->setObjectName(QStringLiteral("channelHeaderTextPopoverContent"));
    content->setMinimumWidth(0);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    layout->addWidget(content);
    scrollArea->setWidget(container);

    connect(content, &MessageContentWidget::dimensionsChanged, this, [this] {
        configurePopoverLinks();
        if (!popover || !popover->isVisible()) {
            return;
        }
        QTimer::singleShot(0, this, [this] {
            if (popover && popover->isVisible()) {
                updatePopoverGeometry(true);
            }
        });
    });

    scrollArea->hide();
    // The popup owns hover as one region. Listening to Leave on its viewport,
    // container and content causes false exits while the pointer merely moves
    // between nested children (or onto the scrollbar), which can start a hide
    // while the user is still inside the popup.
    scrollArea->installEventFilter(this);

    popover = scrollArea;
    popoverContainer = container;
    popoverContent = content;

    auto* animation = new QPropertyAnimation(scrollArea, "geometry", this);
    animation->setObjectName(QStringLiteral("channelHeaderTextPopoverAnimation"));
    animation->setDuration(PopoverAnimationDurationMs);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QPropertyAnimation::finished,
            this, &ChannelHeaderTextLabel::schedulePopoverHide);
    popoverAnimation = animation;
}

void ChannelHeaderTextLabel::updatePopoverMargins()
{
    if (!popover || !popoverContainer || !popoverContainer->layout()) {
        return;
    }

    QWidget* host = popover->parentWidget();
    if (!host) {
        return;
    }

    // The QScrollArea itself cannot provide content padding: its contents
    // margins do not inset the viewport. Put the spacing on the scrollable
    // container layout so it affects the actual rendered message and scroll
    // extent. Keep the first line at the exact same x as the collapsed topic.
    //
    // QLabel rich text is not painted directly at contentsRect().left().
    // QLabelPrivate::documentRect() additionally applies QLabel::margin()
    // and the effective indent. Mirror that public part of the calculation
    // here so hover expansion targets the actual collapsed text origin.
    QRect collapsedDocumentRect = contentsRect();
    const int labelMargin = margin();
    collapsedDocumentRect.adjust(
        labelMargin, labelMargin, -labelMargin, -labelMargin);

    int effectiveIndent = indent();
    if (effectiveIndent < 0 && frameWidth()) {
        effectiveIndent = fontMetrics().horizontalAdvance(QLatin1Char('x')) / 2
            - labelMargin;
    }
    if (effectiveIndent > 0 && (alignment() & Qt::AlignLeft)) {
        collapsedDocumentRect.setLeft(
            collapsedDocumentRect.left() + effectiveIndent);
    }

    const int textLeft = mapTo(
        host, QPoint(collapsedDocumentRect.left(), 0)).x();

    // MessageContentWidget itself has zero layout margins, but QTextBrowser's
    // actual text origin can still be inset by the active QStyle/viewport and
    // document layout. Compensate that runtime inset instead of baking in a
    // theme-specific pixel adjustment.
    int renderedTextInset = 0;
    if (popoverContent) {
        if (auto* browser = popoverContent->findChild<QTextBrowser*>(
                QStringLiteral("messageRichText"))) {
            renderedTextInset = browser->viewport()
                                    ->mapTo(popoverContent, QPoint(0, 0))
                                    .x();
            const QTextBlock firstBlock = browser->document()->firstBlock();
            if (firstBlock.isValid() && browser->document()->documentLayout()) {
                renderedTextInset += static_cast<int>(std::lround(
                    browser->document()->documentLayout()
                        ->blockBoundingRect(firstBlock)
                        .left()));
            }
        }
    }

    const int leftMargin = std::max(0, textLeft - renderedTextInset);
    popoverContainer->layout()->setContentsMargins(
        leftMargin, 0, PopoverHorizontalMargin, PopoverBottomMargin);
    popoverContainer->updateGeometry();
}

void ChannelHeaderTextLabel::configurePopoverLinks()
{
    if (!popoverContent) {
        return;
    }

    const auto browsers = popoverContent->findChildren<QTextBrowser*>();
    for (QTextBrowser* browser : browsers) {
        browser->setOpenExternalLinks(false);
        if (browser->property("channelHeaderLinkConfigured").toBool()) {
            continue;
        }
        browser->setProperty("channelHeaderLinkConfigured", true);
        connect(browser, &QTextBrowser::anchorClicked, this,
                [this](const QUrl& url) { openLink(url); });
    }
}

QRect ChannelHeaderTextLabel::targetPopoverGeometry() const
{
    if (!popover || !popoverContent) {
        return QRect();
    }

    QWidget* host = popover->parentWidget();
    if (!host) {
        return QRect();
    }

    const QPoint labelPos = mapTo(host, QPoint(0, 0));
    const int top = std::max(0, labelPos.y());
    const int popupWidth = std::max(1, host->width());
    const int availableHeight = std::max(1, host->height() - top);
    const int halfChatHeight = std::max(height(), host->height() / 2);
    const int maximumHeight = std::min(availableHeight, halfChatHeight);
    const int contentHeight = std::max(height(), popoverContent->sizeHint().height());
    const QMargins margins = popoverContainer && popoverContainer->layout()
        ? popoverContainer->layout()->contentsMargins()
        : QMargins();
    const int naturalHeight = contentHeight + margins.top() + margins.bottom();
    const int popupHeight = std::min(naturalHeight, maximumHeight);

    return QRect(0, top, popupWidth, std::max(1, popupHeight));
}

void ChannelHeaderTextLabel::updatePopoverGeometry(bool animate)
{
    if (!popover || !popoverContent) {
        return;
    }

    QWidget* host = popover->parentWidget();
    if (!host) {
        return;
    }

    updatePopoverMargins();

    // This is an expansion of the ChatArea header rather than an independent
    // card. Use the exact panel background and the entire chat-side width.
    QPalette popoverPalette = popover->palette();
    const QColor panelBackground = host->palette().color(QPalette::Window);
    popoverPalette.setColor(QPalette::Base, panelBackground);
    popoverPalette.setColor(QPalette::Window, panelBackground);
    popover->setPalette(popoverPalette);

    const int contentHeight = std::max(height(), popoverContent->sizeHint().height());
    popoverContent->setMinimumHeight(contentHeight);

    const QRect target = targetPopoverGeometry();
    if (!target.isValid()) {
        return;
    }

    if (animate && popover->isVisible()) {
        animatePopoverTo(target, false);
    } else {
        if (popoverAnimation) {
            popoverAnimation->stop();
        }
        hideAfterAnimation = false;
        popover->setGeometry(target);
    }
    popover->raise();
}

void ChannelHeaderTextLabel::animatePopoverTo(const QRect& target, bool hideAfter)
{
    if (!popover || !popoverAnimation || !target.isValid()) {
        return;
    }

    popoverAnimation->stop();
    hideAfterAnimation = hideAfter;

    if (popover->geometry() == target) {
        if (hideAfterAnimation) {
            schedulePopoverHide();
        }
        return;
    }

    popoverAnimation->setStartValue(popover->geometry());
    popoverAnimation->setEndValue(target);
    popoverAnimation->start();
}

void ChannelHeaderTextLabel::schedulePopoverHide()
{
    if (!hideAfterAnimation) {
        return;
    }

    // Never hide a QWidget synchronously from the animation callback. A mouse
    // event may still be dispatching through one of the popup's descendants.
    // Defer the visibility change one turn; showPopover() cancels it simply by
    // clearing hideAfterAnimation when the pointer comes back meanwhile.
    QTimer::singleShot(0, this, [this] {
        if (!hideAfterAnimation) {
            return;
        }
        if (popover) {
            popover->hide();
        }
        hideAfterAnimation = false;
    });
}

void ChannelHeaderTextLabel::showPopover()
{
    hideTimer.stop();
    if (!isOverflowing()) {
        return;
    }

    ensurePopover();
    if (!popover || !popoverContent) {
        return;
    }

    if (popoverAnimation) {
        popoverAnimation->stop();
    }
    hideAfterAnimation = false;

    // Re-entering while the panel is collapsing must only reverse the animation.
    // Rebuilding MessageContentWidget here destroys QTextBrowser children that
    // may still be the current QApplication mouse target, which can leave Qt
    // dereferencing a dead QWidget later in sendMouseEvent/mapFromGlobal.
    if (popoverSourceText != sourceText) {
        popoverContent->setMinimumHeight(0);
        popoverContent->setMessage(sourceText);
        popoverSourceText = sourceText;
        configurePopoverLinks();
    }
    updatePopoverMargins();

    QWidget* host = popover->parentWidget();
    if (!host) {
        return;
    }
    const QPoint labelPos = mapTo(host, QPoint(0, 0));
    const QRect collapsed(0, std::max(0, labelPos.y()),
                          std::max(1, host->width()), std::max(1, height()));

    if (!popover->isVisible()) {
        popover->setGeometry(collapsed);
        popover->show();
    }
    popover->raise();

    // The message renderer finalizes wrapped text heights on the next event-loop
    // turn. Start with whatever is already known, then smoothly retarget once the
    // final dimensions arrive instead of flashing a full-size panel immediately.
    updatePopoverGeometry(true);
    QTimer::singleShot(0, this, [this] {
        if (popover && popover->isVisible()) {
            updatePopoverGeometry(true);
        }
    });
}

void ChannelHeaderTextLabel::hidePopoverSoon()
{
    hideTimer.start();
}

void ChannelHeaderTextLabel::hidePopover()
{
    hideTimer.stop();
    if (!popover || !popover->isVisible()) {
        return;
    }

    QRect collapsed = popover->geometry();
    collapsed.setHeight(std::max(1, height()));
    animatePopoverTo(collapsed, true);
}

void ChannelHeaderTextLabel::hidePopoverImmediately()
{
    hideTimer.stop();
    hideAfterAnimation = false;
    if (popoverAnimation) {
        popoverAnimation->stop();
    }
    if (popover) {
        popover->hide();
    }
}

void ChannelHeaderTextLabel::openLink(const QUrl& url)
{
    if (!url.isValid()) {
        return;
    }
    if (linkHandler) {
        linkHandler(url);
        return;
    }

    // The header is specific to ChatArea, so use its semantic navigation as the
    // default route. AppNavigationService keeps external URLs in the browser and
    // handles local channel/DM/permalink URLs inside the application.
    for (QWidget* host = parentWidget(); host; host = host->parentWidget()) {
        if (auto* area = qobject_cast<ChatArea*>(host)) {
            AppNavigationService::instance(area->getBackend()).openUrl(url);
            return;
        }
    }
    QDesktopServices::openUrl(url);
}

bool ChannelHeaderTextLabel::eventFilter(QObject* watched, QEvent* event)
{
    const bool isLabel = watched == this;
    const bool isPopover = popover && watched == popover.data();

    if (isLabel || isPopover) {
        switch (event->type()) {
        case QEvent::Enter:
            hideTimer.stop();
            if (isLabel || (isPopover && hideAfterAnimation)) {
                showPopover();
            }
            break;
        case QEvent::Leave:
            hidePopoverSoon();
            break;
        case QEvent::Resize:
            if (isLabel && popover && popover->isVisible()) {
                if (isOverflowing()) {
                    updatePopoverGeometry(true);
                } else {
                    hidePopover();
                }
            }
            break;
        case QEvent::Hide:
            if (isLabel) {
                hidePopoverImmediately();
            }
            break;
        case QEvent::FontChange:
            if (isLabel) {
                updateCollapsedHeight();
                if (!sourceText.isEmpty()) {
                    setText(sourceText);
                }
            }
            break;
        case QEvent::PaletteChange:
        case QEvent::ApplicationPaletteChange:
            if (popover && popover->isVisible()) {
                updatePopoverGeometry(false);
            }
            break;
        default:
            break;
        }
    }

    return QLabel::eventFilter(watched, event);
}

} // namespace Mattermost
