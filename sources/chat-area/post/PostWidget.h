/**
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

#include <functional>

#include <QFont>
#include <QMargins>
#include <QPushButton>
#include <QSet>
#include <memory>
#include <vector>

#include "MessageContentWidget.h"
#include "backend/AttachmentService.h"
#include "backend/PostResidencyLease.h"
#include "backend/types/BackendPost.h"

class QCheckBox;
class QContextMenuEvent;
class QEvent;
class QFrame;
class QGraphicsOpacityEffect;
class QHBoxLayout;
class QLabel;
class QMouseEvent;
class QMoveEvent;
class QPaintEvent;
class QPropertyAnimation;
class QResizeEvent;
class QTimer;

namespace Ui {
class PostWidget;
}

namespace Mattermost {

class Backend;
class BackendUser;
class BusyIndicatorWidget;
class PostQuoteFrame;
class QuotedPostPreview;
class PostAttachmentList;
class PostReactionList;
class PostPoll;
class ChatArea;
class ChatLogWidget;
class KTalkMeetingWidget;
class ReactionQuickBarController;
class ThreadSummaryWidget;

class PostWidget: public QWidget
{
    Q_OBJECT

public:
    enum class PresentationMode {
        Interactive,
        ReadOnlySnapshot,
        Pending,
    };

    explicit PostWidget(
        Backend& backend, BackendPost& post, QWidget* parent,
        ChatArea* chatArea, BackendPost* lastRootPost,
        PresentationMode presentationMode = PresentationMode::Interactive,
        std::shared_ptr<BackendPost> postLease = {});
    ~PostWidget();
public:

    enum FormatType {
		messageOnly,
		entirePost
    };

    void setEdited (const QString& message);
    void updateReactions ();

    void markAsDeleted ();

    QString getSelectedText ();

    QString getMessageTimeString (uint64_t timestamp) const;
    QString formatMessageText(const QString& str) const;
    QString formatForClipboardSelection (FormatType formatType) const;
    void clearTextSelection();
    void setWholeMessageSelectionMode(bool enabled);
    void setWholeMessageSelected(bool selected);
    void setHovered(bool hovered, bool immediate = false);

    /**
     * Presentation-only grouping state. This is deliberately mutable: source
     * paging/filtering/removal can change which post immediately precedes this
     * widget without recreating the post itself.
     */
    void setAuthorRunContinuation(bool continuation);
    bool authorRunContinuation() const { return authorRunContinuation_; }

    bool wholeMessageSelectionMode() const { return wholeMessageSelectionMode_; }
    bool wholeMessageSelected() const { return wholeMessageSelected_; }

    void clearMessageText ();

    void addThreadButton();
    Backend& getBackend() const { return backend_; }

    void prepareInlineAttachmentContext()
    {
        if (!messageContent) {
            return;
        }

        QSet<QString> imageFileIds;
        for (const BackendFile& file : post.files) {
            if (file.mimeType.startsWith(
                    QStringLiteral("image"), Qt::CaseInsensitive)) {
                imageFileIds.insert(file.id);
            }
        }

        Backend* sourceBackend = &backend_;
        messageContent->setInlineAttachmentContext(
            imageFileIds,
            [sourceBackend](
                const QString& fileId,
                bool thumbnail,
                MessageContentWidget::InlineImageDataCallback callback) {
                AttachmentService& service =
                    AttachmentService::instance(*sourceBackend);
                if (thumbnail) {
                    service.retrieveThumbnail(fileId, callback);
                } else {
                    service.retrievePreview(fileId, callback);
                }
            });

        // setInlineAttachmentContext() rematerializes the QTextBrowser children
        // when a message is already present. Reattach PostWidget-owned link,
        // mention and context-menu behavior to those replacement browsers.
        connectMessageLinks();
    }

    bool isAttachmentRenderedInline(const QString& fileId) const
    {
        return messageContent
            && messageContent->inlineAttachmentFileIds().contains(fileId);
    }

    void setPendingDeliveryPresentation(const QString& statusText,
                                        bool failed,
                                        std::function<void()> retry,
                                        std::function<void()> cancel);

    BackendPost&						post;
    QString								hoveredLink;
    QPushButton*						threadButton;

public slots:
    void openThreadWindow();

private slots:
    void on_authorAvatar_clicked();

signals:
	void dimensionsChanged ();
    void wholeMessageSelectionToggled(const QString& postId, bool selected);
    void markUnreadRequested(const QString& postId);

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void moveEvent(QMoveEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    friend class ReactionQuickBarController;

    void showPostContextMenu(const QPoint& globalPos);
    void animateHoverActions(bool visible, bool immediate = false);
    void positionHoverActions();
    void updateHoverActionsPalette();
    void updateTimestampPresentation();
    void updateTimestampPalette();
    bool usesRelativeTimestamp() const;
    void setAuthor(Backend& backendInstance, const BackendUser* user);
    void updateAuthorAvatar();
    void createReactionList();
    void connectReactionActions();
    void ensureEngagementRow();
    void updateEngagementRow();
    void updateEngagementRowMetrics();
    void connectMessageLinks();
    void refreshMentionLinks();
    void refreshPermalinkPreviews();
    void openUserProfile(const QString& username);
    void openGroupMention(const QString& groupId);
    void applyChatFont(const QString& serializedFont,
                       bool notifyGeometry = true);
    QString mentionTeamId() const;

    Backend&                            backend_;
    std::shared_ptr<BackendPost>       postLease_;
    PostResidencyLease                 residencyLease;
    Ui::PostWidget*						ui;
    std::unique_ptr<PostQuoteFrame>		quoteFrame;
    std::unique_ptr<QuotedPostPreview>    quotedReplyPreview;
    std::vector<std::unique_ptr<QuotedPostPreview>> permalinkPreviews;
    std::unique_ptr<PostAttachmentList>	attachments;
    std::unique_ptr<PostPoll>			poll;
    std::unique_ptr<PostReactionList>	reactions;
    std::unique_ptr<KTalkMeetingWidget> ktalkMeeting_;
    MessageContentWidget*				messageContent;
    ChatArea*				parentChatArea;
    PresentationMode                    presentationMode_ = PresentationMode::Interactive;
    ThreadSummaryWidget*                threadSummary = nullptr;
    QWidget*                            engagementRow_ = nullptr;
    QHBoxLayout*                        engagementLayout_ = nullptr;
    QCheckBox*                         wholeMessageCheck_ = nullptr;
    BusyIndicatorWidget*               pendingDeliveryIndicator_ = nullptr;
    bool                               _pendingIndicatorOverlay = false;
    QWidget*                           pendingDeliveryRow_ = nullptr;
    QLabel*                            pendingDeliveryLabel_ = nullptr;
    QPushButton*                       pendingRetryButton_ = nullptr;
    QPushButton*                       pendingCancelButton_ = nullptr;
    std::function<void()>              pendingRetry_;
    std::function<void()>              pendingCancel_;
    QFrame*                            hoverActions_ = nullptr;
    QWidget*                           reactionQuickBarSlot_ = nullptr;
    QPushButton*                       threadAffordance_ = nullptr;
    QPushButton*                       reactionAffordance_ = nullptr;
    QPushButton*                       saveAffordance_ = nullptr;
    QPushButton*                       moreAffordance_ = nullptr;
    QLabel*                            continuationTime_ = nullptr;
    QGraphicsOpacityEffect*            hoverActionsOpacity_ = nullptr;
    QPropertyAnimation*                hoverActionsAnimation_ = nullptr;
    QTimer*                            hoverActionsHideTimer_ = nullptr;
    QTimer*                            timestampRefreshTimer_ = nullptr;
    bool                               hoverActionsWanted_ = false;
    bool                               hovered_ = false;
    bool                               wholeMessageSelectionMode_ = false;
    bool                               wholeMessageSelected_ = false;
    bool                                authorRunContinuation_ = false;
    QMargins                             normalRowMargins_;
    ChatLogWidget* chatLog() const;
    QFont                              chatFont_;
};

} /* namespace Mattermost */