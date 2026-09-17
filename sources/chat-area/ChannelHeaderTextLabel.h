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

#pragma once

#include <functional>

#include <QLabel>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QUrl>

class QPropertyAnimation;
class QScrollArea;

namespace Mattermost {

class MessageContentWidget;

/**
 * Compact channel-header text with Mattermost-like Markdown hover expansion.
 *
 * The collapsed label remains one line high. Overflowing/multiline text is
 * shown in an overlay using the same renderer as ordinary messages, so code
 * blocks and other Markdown stay visually consistent without changing the chat
 * layout or moving the currently visible posts.
 */
class ChannelHeaderTextLabel final: public QLabel
{
    Q_OBJECT
public:
    using LinkHandler = std::function<void(const QUrl&)>;

    explicit ChannelHeaderTextLabel(QWidget* parent = nullptr);

    // QLabel::setText() is not virtual, but ui_ChatArea stores this concrete
    // type, so ChatArea's existing calls resolve to this formatting wrapper.
    void setText(const QString& text);
    void setLinkHandler(LinkHandler handler);
    void setPresenceRoutingEnabled(bool enabled) { presenceRoutingEnabled = enabled; }

    // Rich-text QLabel uses its unwrapped document width as a minimum hint.
    // A topic must never dictate a thread-window or chat-pane width.
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private slots:
    // Slots so the hover state transitions can be exercised directly in GUI
    // tests without synthesizing platform mouse events under the offscreen QPA.
    void showPopover();
    void hidePopover();

private:
    bool isOverflowing() const;
    void ensurePopover();
    void updatePopoverGeometry(bool animate);
    QRect targetPopoverGeometry() const;
    void updatePopoverMargins();
    void configurePopoverLinks();
    void animatePopoverTo(const QRect& target, bool hideAfterAnimation);
    void schedulePopoverHide();
    void hidePopoverSoon();
    void hidePopoverImmediately();
    void updateCollapsedHeight();
    void openLink(const QUrl& url);

    QString sourceText;
    QString formattedText;
    QString popoverSourceText;
    QPointer<QScrollArea> popover;
    QPointer<QWidget> popoverContainer;
    QPointer<MessageContentWidget> popoverContent;
    QPointer<QPropertyAnimation> popoverAnimation;
    QTimer hideTimer;
    LinkHandler linkHandler;
    bool presenceRoutingEnabled = true;
    bool hideAfterAnimation = false;
};

} // namespace Mattermost
