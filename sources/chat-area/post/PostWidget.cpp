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

#include "PostWidget.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QTextBlock>
#include <QTextDocument>
#include <QSignalBlocker>
#include <QPropertyAnimation>
#include <QPainter>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QIcon>
#include <QLabel>
#include <QCheckBox>
#include <QCursor>
#include <QDateTime>
#include <QDebug>
#include <QDrag>
#include <QEvent>
#include <QFrame>
#include <QFontMetrics>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVariant>

#include "MessageContentWidget.h"
#include "Settings.h"
#include "MessageFormatter.h"
#include "PostPermalinkUtils.h"
#include "PostQuoteFrame.h"
#include "PostTimestampPresentation.h"
#include "PostDateBoundaryPresentation.h"
#include "ReactionChipStyle.h"
#include "ThreadSummaryWidget.h"
#include "UserMentionLinkifier.h"
#include "attachments/AttachedImageFile.h"
#include "attachments/AttachmentBatchSave.h"
#include "attachments/PostAttachmentList.h"
#include "attachments/PostPoll.h"
#include "backend/Backend.h"
#include "backend/MentionGroupService.h"
#include "backend/PostProps.h"
#include "backend/PostRepository.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "chat-area/ChatArea.h"
#include "chat-area/ChatLogWidget.h"
#include "chat-area/PostListWidget.h"
#include "chat-area/QuotedPostPreview.h"
#include "chat-area/QuotedReplyController.h"
#include "chat-area/QuotedReplyFormat.h"
#include "chat-area/ThreadWindowTitle.h"
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"
#include "info-dialogs/UserProfileDialog.h"
#include "integrations/KTalkMeetingWidget.h"
#include "navigation/AppNavigationService.h"
#include "options/MLOptions.h"
#include "reactions/PostReactionList.h"
#include "ui/AvatarUtils.h"
#include "ui/BusyIndicator.h"
#include "ui/IconUtils.h"
#include "ui/EmojiFont.h"
#include "ui_PostWidget.h"

namespace Mattermost {

namespace {

QString internalLinkValue(const QUrl& url)
{
    QString value = url.path();
    while (value.startsWith(QLatin1Char('/'))) {
        value.remove(0, 1);
    }
    return value;
}

QString quotedPostId(const BackendPost& post)
{
    return post.props.toObject()
        .value(QString::fromLatin1(PostProps::ReplyToPostId)).toString();
}

QString displayMessage(const BackendPost& post, const QString& wireMessage)
{
    if (post.isDeleted) {
        return post.poll ? QStringLiteral("(Poll deleted)")
                         : QStringLiteral("(Message deleted)");
    }
    return quotedPostId(post).isEmpty()
        ? wireMessage : QuotedReplyFormat::stripFallback(wireMessage);
}


void updateAuthorNameColor(QLabel* authorName, bool ownPost,
                           const QPalette& basePalette)
{
    if (!authorName) {
        return;
    }

    QColor color = basePalette.color(QPalette::WindowText);
    if (ownPost) {
        const QColor blue(Qt::blue);
        const auto mix = [](int text, int accent) {
            return (text * 3 + accent * 2) / 5;
        };
        color = QColor(mix(color.red(), blue.red()),
                       mix(color.green(), blue.green()),
                       mix(color.blue(), blue.blue()));
    }

    QPalette authorPalette = authorName->palette();
    authorPalette.setColor(QPalette::WindowText, color);
    authorName->setPalette(authorPalette);
}

} // namespace

PostWidget::PostWidget(Backend& backend,
                       BackendPost& post,
                       QWidget* parent,
                       ChatArea* chatArea,
                       BackendPost* lastRootPost,
                       PresentationMode presentationMode,
                       std::shared_ptr<BackendPost> postLease)
    : QWidget(parent)
    , post(post)
    , threadButton(nullptr)
    , backend_(backend)
    , postLease_(std::move(postLease))
    , residencyLease(PostRepository::instance(backend).leasePost(post))
    , ui(new Ui::PostWidget)
    , messageContent(nullptr)
    , parentChatArea(chatArea)
    , presentationMode_(presentationMode)
{
	ui->setupUi(this);
    normalRowMargins_ = ui->horizontalLayout_2->contentsMargins();
    // postRootLayout has zero spacing for day dividers. Qt inherits that zero
    // for this nested row unless explicitly overridden, placing text flush
    // against the 48px avatar. Match the existing left avatar inset.
    ui->horizontalLayout_2->setSpacing(normalRowMargins_.left());

    // Match Mattermost header geometry: author and timestamp form one compact
    // left-aligned cluster, with the remaining width trailing after them.
    ui->authorName->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    ui->time->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    ui->time->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    ui->horizontalLayout->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Font-sensitive utility widgets (pending delivery, reactions, thread
    // summary) must be constructed against the final chat font. Constructing
    // them first with QApplication/default metrics and applying CHAT_FONT only
    // at the end gives the row a transient larger sizeHint, which LongList can
    // legitimately measure before the subsequent FontChange/layout settles.
    auto* chatFontOption = MLOptions::instance()->optionObject<QString>(
        CHAT_FONT, font().toString());
    applyChatFont(chatFontOption->value().toString(), false);

    connect(&backend_.emojiRegistry(),
            &EmojiRegistry::customEmojiAdded,
            this, [this](const QString& name) {
        // Reaction identity is already complete in BackendPost. Registry changes
        // affect presentation only, so repaint a matching named reaction without
        // rewriting its semantic key.
        if (this->post.reactions.find(name) != this->post.reactions.end()) {
            updateReactions();
        }
    });

    wholeMessageCheck_ = new QCheckBox(this);
    wholeMessageCheck_->setToolTip(tr("Select message"));
    wholeMessageCheck_->setAccessibleName(tr("Select message"));
    wholeMessageCheck_->setVisible(false);
    ui->horizontalLayout_2->insertWidget(0, wholeMessageCheck_, 0, Qt::AlignTop);
    connect(wholeMessageCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        wholeMessageSelected_ = checked;
        update();
        emit wholeMessageSelectionToggled(this->post.id, checked);
    });

    if (presentationMode_ == PresentationMode::Interactive) {
        if (!postListWidget()) {
        // Standalone post widgets retain their local action surface. Rows in
        // PostListWidget use its one shared, viewport-owned toolbar instead.
        // Post actions are an overlay, not part of row geometry. Parent the
        // surface to the LongList viewport so it may float slightly above this
        // row without being clipped by PostWidget.
        QWidget* overlayHost = parentWidget() ? parentWidget() : this;
        hoverActions_ = new QFrame(overlayHost);
        hoverActions_->setObjectName(QStringLiteral("postHoverActions"));
        hoverActions_->setFrameShape(QFrame::NoFrame);
        hoverActions_->setAutoFillBackground(false);
        updateHoverActionsPalette();
        hoverActions_->hide();
        hoverActions_->installEventFilter(this);

        // Keep the ranked-reaction area structurally separate from the normal
        // action layout. Its permanent zero-width slot can grow leftward
        // without ever changing spacing or local geometry of the ordinary
        // buttons.
        auto* toolbarLayout = new QHBoxLayout(hoverActions_);
        toolbarLayout->setContentsMargins(3, 2, 3, 2);
        toolbarLayout->setSpacing(0);

        reactionQuickBarSlot_ = new QWidget(hoverActions_);
        reactionQuickBarSlot_->setObjectName(
            QStringLiteral("reactionQuickBarSlot"));
        reactionQuickBarSlot_->setMinimumWidth(0);
        reactionQuickBarSlot_->setMaximumWidth(0);
        reactionQuickBarSlot_->setSizePolicy(
            QSizePolicy::Preferred, QSizePolicy::Preferred);
        auto* quickSlotLayout = new QHBoxLayout(reactionQuickBarSlot_);
        quickSlotLayout->setContentsMargins(0, 0, 0, 0);
        quickSlotLayout->setSpacing(0);
        toolbarLayout->addWidget(reactionQuickBarSlot_);

        auto* actionsContainer = new QWidget(hoverActions_);
        auto* actionsLayout = new QHBoxLayout(actionsContainer);
        actionsLayout->setContentsMargins(0, 0, 0, 0);
        actionsLayout->setSpacing(1);
        toolbarLayout->addWidget(actionsContainer);

        const auto makeActionButton =
            [this, actionsLayout, actionsContainer](const QIcon& icon,
                                                    const QString& text,
                                                    const QString& tooltip) {
                auto* button = new QPushButton(actionsContainer);
                button->setFlat(true);
                button->setFixedSize(28, 28);
                button->setCursor(Qt::PointingHandCursor);
                if (!icon.isNull()) {
                    button->setIcon(icon);
                } else {
                    button->setText(text);
                }
                button->setToolTip(tooltip);
                button->setAccessibleName(tooltip);
                button->installEventFilter(this);
                actionsLayout->addWidget(button);
                return button;
            };

        // Reaction is deliberately the leftmost action: the ranked
        // quick-reaction surface opens below this button and should not render
        // visually underneath another toolbar action.
        reactionAffordance_ = makeActionButton(
            IconUtils::symbolicIcon(QStringLiteral(":/icons/emoji")),
            QString(), tr("Add reaction"));
        reactionAffordance_->setProperty(
            "matterleastPostOwner",
            QVariant::fromValue(static_cast<QObject*>(this)));
        connect(reactionAffordance_, &QPushButton::clicked, this, [this] {
            showEmojiDialog([this](Emoji emoji) {
                backend_.addPostReaction(this->post.id, emoji.name);
            });
        });

        const bool canThread =
            parentChatArea && !parentChatArea->isThread && post.root_id.isEmpty();
        if (canThread) {
            threadAffordance_ = makeActionButton(
                IconUtils::symbolicIcon(QStringLiteral(":/icons/message-balloon")),
                QString(), tr("Open thread"));
            connect(threadAffordance_, &QPushButton::clicked,
                    this, &PostWidget::openThreadWindow);
        }

        saveAffordance_ = makeActionButton(
            IconUtils::symbolicIcon(QStringLiteral(":/icons/bookmark")),
            QString(), backend_.isPostFlagged(post.id)
                ? tr("Remove from saved") : tr("Save message"));
        connect(saveAffordance_, &QPushButton::clicked, this, [this] {
            const BackendUserPreferences pref {
                QStringLiteral("flagged_post"), this->post.id, QStringLiteral("true")};
            if (backend_.isPostFlagged(this->post.id)) {
                backend_.deleteUserPreferences(pref);
            } else {
                backend_.updateUserPreferences(pref);
            }
        });
        connect(&backend_, &Backend::onFlaggedPostChanged, this,
                [this](const QString& postId, bool flagged) {
            if (postId == this->post.id && saveAffordance_)
                saveAffordance_->setToolTip(
                    flagged ? tr("Remove from saved") : tr("Save message"));
        });

        moreAffordance_ = makeActionButton(
            IconUtils::symbolicIcon(QStringLiteral(":/icons/more-horizontal")),
            QString(), tr("More actions"));
        connect(moreAffordance_, &QPushButton::clicked, this, [this] {
            if (moreAffordance_) {
                showPostContextMenu(moreAffordance_->mapToGlobal(
                    QPoint(0, moreAffordance_->height())));
            }
        });

        hoverActionsOpacity_ = new QGraphicsOpacityEffect(hoverActions_);
        hoverActionsOpacity_->setOpacity(0.0);
        hoverActions_->setGraphicsEffect(hoverActionsOpacity_);
        hoverActionsAnimation_ = new QPropertyAnimation(
            hoverActionsOpacity_, "opacity", this);
        hoverActionsAnimation_->setDuration(120);
        connect(hoverActionsAnimation_, &QPropertyAnimation::finished,
                this, [this] {
            if (hoverActions_ && !hoverActionsWanted_) {
                hoverActions_->hide();
            }
        });

        hoverActionsHideTimer_ = new QTimer(this);
        hoverActionsHideTimer_->setSingleShot(true);
        hoverActionsHideTimer_->setInterval(160);
        connect(hoverActionsHideTimer_, &QTimer::timeout, this, [this] {
            if (hovered_ || (hoverActions_ && hoverActions_->underMouse())) {
                return;
            }
            hoverActionsWanted_ = false;
            if (continuationTime_) {
                continuationTime_->hide();
            }
            if (!hoverActions_ || !hoverActionsOpacity_
                || !hoverActionsAnimation_) {
                return;
            }
            hoverActionsAnimation_->stop();
            hoverActionsAnimation_->setStartValue(
                hoverActionsOpacity_->opacity());
            hoverActionsAnimation_->setEndValue(0.0);
            hoverActionsAnimation_->start();
        });

        }

        continuationTime_ = new QLabel(this);
        continuationTime_->setObjectName(QStringLiteral("continuationTime"));
        continuationTime_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        continuationTime_->setAttribute(Qt::WA_TransparentForMouseEvents);
        continuationTime_->setFixedWidth(48);
        continuationTime_->setFont(ui->time->font());
        continuationTime_->hide();
    }
	ui->authorAvatar->setFrameShape(QFrame::NoFrame);
	ui->authorName->setText(post.getDisplayAuthorName());

    if (presentationMode_ == PresentationMode::Pending) {
        pendingDeliveryIndicator_ = new BusyIndicatorWidget(this);
        pendingDeliveryIndicator_->setObjectName(QStringLiteral("pendingDeliveryIndicator"));
        pendingDeliveryIndicator_->setFixedSize(
            12, ReactionChipStyle::chipHeight(chatFont_));
        pendingDeliveryIndicator_->setToolTip(tr("Sending"));
        pendingDeliveryIndicator_->setAccessibleName(tr("Message is sending"));

        // Use the same right-side utility slot that authoritative root posts
        // use for ThreadSummaryWidget. A pending post has no server post ID yet,
        // so it cannot expose the thread action itself, but delivery state
        // belongs in that utility area rather than expanding the author name.
        ui->time->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
        ui->horizontalLayout->insertStretch(1, 1);
        ui->horizontalLayout->insertWidget(
            2, pendingDeliveryIndicator_, 0, Qt::AlignVCenter);
        const int indicatorTimeGap =
            ui->time->fontMetrics().averageCharWidth();
        ui->horizontalLayout->insertSpacing(3, indicatorTimeGap);
    }

    updateAuthorNameColor(ui->authorName, post.isOwnPost(), palette());

	messageContent = new MessageContentWidget(this);
    messageContent->setEmojiRegistry(&backend_.emojiRegistry());
	const int messageIndex = ui->verticalLayout->indexOf(ui->message);
	ui->verticalLayout->removeWidget(ui->message);
	ui->message->hide();
	ui->verticalLayout->insertWidget(messageIndex, messageContent);
	connect(messageContent, &MessageContentWidget::dimensionsChanged,
	        this, &PostWidget::dimensionsChanged);
	connect(messageContent, &MessageContentWidget::contentRebuilt,
            this, &PostWidget::connectMessageLinks);
	messageContent->setMessage(displayMessage(post, post.message));
	refreshPermalinkPreviews();
    updateTimestampPresentation();
    updateTimestampPalette();

    if (parentChatArea && parentChatArea->isThread) {
        // Every materialized thread row observes the same persistent setting.
        // Updating a label in place preserves scroll anchors and avoids
        // reloading any thread history (including detached thread windows).
        auto* formatOption = MLOptions::instance()->optionObject<bool>(
            THREAD_RELATIVE_TIMESTAMPS, THREAD_RELATIVE_TIMESTAMPS_DEFAULT);
        timestampRefreshTimer_ = new QTimer(this);
        timestampRefreshTimer_->setObjectName(
            QStringLiteral("threadTimestampRefreshTimer"));
        timestampRefreshTimer_->setSingleShot(true);
        const auto updateRefreshTimer = [this] {
            if (usesRelativeTimestamp() && !fullTimestampVisible_) {
                timestampRefreshTimer_->start(
                    PostTimestampPresentation::threadRefreshIntervalMs(
                        static_cast<qint64>(this->post.create_at)));
            } else {
                timestampRefreshTimer_->stop();
            }
        };
        connect(timestampRefreshTimer_, &QTimer::timeout, this,
                [this, updateRefreshTimer] {
            updateTimestampPresentation();
            updateRefreshTimer();
        });
        connect(formatOption, &MLOptionObject::changed, this,
                [this, updateRefreshTimer](const QVariant&) {
            updateTimestampPresentation();
            updateRefreshTimer();
            ui->horizontalLayout->invalidate();
            updateGeometry();
        });
        updateRefreshTimer();
    }

    if (presentationMode_ != PresentationMode::Pending
        && !post.isDeleted && KTalkMeetingWidget::supports(post)) {
        auto meeting = std::make_unique<KTalkMeetingWidget>(backend_, post, this);
        if (meeting->isValid()) {
            ktalkMeeting_ = std::move(meeting);
            ui->verticalLayout->insertWidget(
                messageIndex + 1, ktalkMeeting_.get(), 0, Qt::AlignLeft);
        }
    }

	connect(messageContent, &MessageContentWidget::linkHovered,
	        this, [this](const QString& link) {
		qDebug() << "Link hovered:" << link;
		hoveredLink = link;
	});
    connect(messageContent, &MessageContentWidget::linkDragRequested,
            this, [this](const QString& link) {
        const QUrl url(link);
        if (!url.isValid() || url.isEmpty()) {
            return;
        }

        // text/uri-list is the interoperable payload browsers expect when a
        // hyperlink is dragged from a page; text/plain keeps address-bar and
        // text drop targets useful as well.
        auto* mimeData = new QMimeData;
        mimeData->setUrls({url});
        mimeData->setText(url.toString());

        QDrag drag(messageContent);
        drag.setMimeData(mimeData);
        drag.exec(Qt::CopyAction);
    });

    const QString teamId = mentionTeamId();
    if (!teamId.isEmpty()) {
        auto& groupService = MentionGroupService::instance(backend);
        connect(&groupService, &MentionGroupService::groupsChanged,
                this, [this, teamId](const QString& changedTeamId) {
            if (changedTeamId == teamId) {
                refreshMentionLinks();
            }
        });
        groupService.ensureTeamGroups(teamId);
    }

    // Profile requests are lazy and may finish after a post widget has already
    // rendered an unresolved user ID. Rebind on every successful profile
    // arrival, including a later request from another view or a reconnect.
    auto& profileService = UserProfileService::instance(backend);
    connect(&profileService, &UserProfileService::profileResolved, this,
            [this](const QString& userId) {
        if (userId != this->post.user_id) {
            return;
        }
        const BackendUser* author = backend_.getStorage().getUserById(userId);
        if (!author) {
            return;
        }
        if (this->post.author == author
            && ui->authorName->text() == this->post.getDisplayAuthorName()) {
            return;
        }
        setAuthor(backend_, author);
    });

	if (post.author) {
		setAuthor(backend, post.author);
	} else if (!post.user_id.isEmpty()) {
		QPointer<PostWidget> guard(this);
		UserProfileService::instance(backend).ensureUser(
			post.user_id, [guard](const BackendUser* user) {
				if (guard && user) {
					guard->setAuthor(guard->backend_, user);
			}
			});
	}

    const QString replyPostId = quotedPostId(post);
    if (!post.isDeleted && !replyPostId.isEmpty() && parentChatArea) {
        BackendPost* quotedPost = parentChatArea->channel.postIdToPost.value(replyPostId, nullptr);
        if (quotedPost && quotedPost != &post) {
            quotedReplyPreview = std::make_unique<QuotedPostPreview>(this, 2);
            quotedReplyPreview->setPost(*quotedPost);
            connect(quotedReplyPreview.get(), &QuotedPostPreview::dimensionsChanged,
                    this, &PostWidget::dimensionsChanged);
            quotedReplyPreview->setActivatedCallback([this, replyPostId] {
                AppNavigationService::instance(backend_).openPost(replyPostId);
            });
            ui->verticalLayout->insertWidget(1, quotedReplyPreview.get());
        } else {
            QPointer<PostWidget> guard(this);
            PostRepository::instance(backend).loadPost(
                replyPostId,
                [guard, replyPostId](const PostRepository::PostResult& result) {
                    if (!guard || !result.success || !guard->parentChatArea
                        || guard->quotedReplyPreview || guard->post.isDeleted) {
                        return;
                    }
                    BackendPost* loaded = guard->parentChatArea->channel.postIdToPost
                        .value(replyPostId, nullptr);
                    if (!loaded || loaded == &guard->post) {
                        return;
                    }

                    guard->quotedReplyPreview =
                        std::make_unique<QuotedPostPreview>(guard, 2);
                    guard->quotedReplyPreview->setPost(*loaded);
                    connect(guard->quotedReplyPreview.get(),
                            &QuotedPostPreview::dimensionsChanged,
                            guard, &PostWidget::dimensionsChanged);
                    guard->quotedReplyPreview->setActivatedCallback(
                        [guard, replyPostId] {
                            if (guard) {
                                AppNavigationService::instance(guard->backend_)
                                    .openPost(replyPostId);
                            }
                        });
                    guard->ui->verticalLayout->insertWidget(
                        1, guard->quotedReplyPreview.get());
                    emit guard->dimensionsChanged();
                });
        }
	} else if (!post.isDeleted && post.rootPost && post.rootPost != lastRootPost) {
		quoteFrame = std::make_unique<PostQuoteFrame>(*post.rootPost,
		                                              backend.getStorage(), this);
		ui->verticalLayout->insertWidget(1, quoteFrame.get(), 0, Qt::AlignLeft);
        const QString rootPostId = post.rootPost->id;
		connect(quoteFrame.get(), &PostQuoteFrame::postClicked, this,
                [this, rootPostId] {
            AppNavigationService::instance(backend_).openPost(rootPostId);
        });
	}

	if (!post.isDeleted && !post.files.empty()) {
		attachments = std::make_unique<PostAttachmentList>(backend, this);
		connect(attachments.get(), &PostAttachmentList::dimensionsChanged,
		        this, &PostWidget::dimensionsChanged);
		ui->verticalLayout->addWidget(attachments.get());
        attachments->setFiles(post.files, post.getDisplayAuthorName());
	}

	if (presentationMode_ != PresentationMode::Pending
        && !post.isDeleted && post.poll) {
		clearMessageText();
		poll = std::make_unique<PostPoll>(backend, post, *post.poll, this);
		ui->verticalLayout->addWidget(poll.get());
	}

    if (presentationMode_ != PresentationMode::Pending) {
        createReactionList();
    }

	if (presentationMode_ != PresentationMode::Pending
        && parentChatArea && !parentChatArea->isThread) {
		addThreadButton();
	}

    connect(chatFontOption, &MLOptionObject::changed, this,
            [this](const QVariant& value) {
        applyChatFont(value.toString());
    });
}

PostWidget::~PostWidget()
{
    // hoverActions_ is parented to the LongList viewport so it can paint
    // outside this row. It is still logically owned by this PostWidget.
    if (hoverActionsAnimation_) {
        hoverActionsAnimation_->stop();
    }
    // The list-owned bar is borrowed only while this row is its active target.
    // It outlives materialized rows and must never be deleted by them.
    if (hoverActions_
        && !hoverActions_->property("matterleastSharedPostHoverActions").toBool()) {
        delete hoverActions_;
    }
    hoverActions_ = nullptr;
	delete ui;
}

void PostWidget::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (!event || (event->type() != QEvent::PaletteChange
                   && event->type() != QEvent::ApplicationPaletteChange)) {
        return;
    }

    updateAuthorAvatar();
    updateAuthorNameColor(ui->authorName, post.isOwnPost(), palette());
    updateHoverActionsPalette();
    updateTimestampPalette();
    update();
    const auto childWidgets = findChildren<QWidget*>();
    for (QWidget* child : childWidgets) {
        if (child) {
            child->update();
        }
    }
    if (QWidget* viewportWidget = parentWidget()) {
        viewportWidget->update();
    }
}

bool PostWidget::eventFilter(QObject* watched, QEvent* event)
{
    QWidget* watchedWidget = qobject_cast<QWidget*>(watched);
    if (event && hoverActions_ && watchedWidget
        && (watchedWidget == hoverActions_
            || hoverActions_->isAncestorOf(watchedWidget))) {
        if (event->type() == QEvent::Enter) {
            if (hoverActionsHideTimer_) {
                hoverActionsHideTimer_->stop();
            }
            animateHoverActions(true);
        } else if (event->type() == QEvent::Leave && !hovered_) {
            if (hoverActionsHideTimer_) {
                hoverActionsHideTimer_->start();
            }
        }
    }

    if (event && event->type() == QEvent::MouseButtonRelease) {
        auto* mouseEvent = static_cast<QMouseEvent*>(event);
        if (mouseEvent->button() == Qt::MiddleButton) {
            auto* viewport = qobject_cast<QWidget*>(watched);
            auto* browser = viewport
                ? qobject_cast<QTextBrowser*>(viewport->parentWidget())
                : nullptr;
            if (browser && browser->viewport() == viewport) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
                const QPoint pos = mouseEvent->position().toPoint();
#else
                const QPoint pos = mouseEvent->pos();
#endif
                const QString anchor = browser->anchorAt(pos);
                if (!anchor.isEmpty()) {
                    const QUrl url(anchor);
                    const bool profileLink =
                        url.scheme() == QStringLiteral("mattermost-user")
                        || url.scheme() == QStringLiteral("mattermost-group");
                    if (!profileLink) {
                        AppNavigationService::instance(backend_).openUrlInTab(url);
                        event->accept();
                        return true;
                    }
                }
            }
        }
    }

    return QWidget::eventFilter(watched, event);
}

void PostWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (!event) {
        return;
    }
    showPostContextMenu(event->globalPos());
    event->accept();
}

void PostWidget::paintEvent(QPaintEvent* event)
{
    QWidget::paintEvent(event);
    if (!wholeMessageSelected_
        && !property("_mmqt_contextMenuActive").toBool()) {
        return;
    }
    QColor selected = palette().color(QPalette::Highlight);
    selected.setAlpha(34);
    QPainter painter(this);
    QRect selectedArea = rect();
    // The date divider is not part of the selected message chrome.
    if (dayBoundaryWidget_ && !dayBoundaryWidget_->isHidden())
        selectedArea.setTop(dayBoundaryWidget_->sizeHint().height());
    painter.fillRect(selectedArea, selected);
}

ChatLogWidget* PostWidget::chatLog() const
{
    return qobject_cast<ChatLogWidget*>(parentWidget() ? parentWidget()->parentWidget() : nullptr);
}

PostListWidget* PostWidget::postListWidget() const
{
    for (QWidget* parent = parentWidget(); parent; parent = parent->parentWidget()) {
        if (auto* list = dynamic_cast<PostListWidget*>(parent)) {
            return list;
        }
    }
    return nullptr;
}

void PostWidget::moveEvent(QMoveEvent* event)
{
    QWidget::moveEvent(event);
    positionHoverActions();
}

void PostWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    positionHoverActions();
}

void PostWidget::setWholeMessageSelectionMode(bool enabled)
{
    if (wholeMessageSelectionMode_ == enabled) {
        return;
    }
    wholeMessageSelectionMode_ = enabled;
    if (wholeMessageCheck_) {
        wholeMessageCheck_->setVisible(enabled);
    }
    if (enabled) {
        clearTextSelection();
    }
    animateHoverActions(hovered_);
    updateGeometry();
    update();
}

void PostWidget::setWholeMessageSelected(bool selected)
{
    wholeMessageSelected_ = selected;
    if (wholeMessageCheck_) {
        const QSignalBlocker blocker(wholeMessageCheck_);
        wholeMessageCheck_->setChecked(selected);
    }
    update();
}

void PostWidget::setHovered(bool hovered, bool immediate)
{
    if (presentationMode_ != PresentationMode::Interactive) {
        hovered_ = false;
        animateHoverActions(false, immediate);
        return;
    }
    if (hovered_ == hovered && !immediate) {
        return;
    }

    hovered_ = hovered;
    if (hovered_) {
        if (hoverActionsHideTimer_) {
            hoverActionsHideTimer_->stop();
        }
        animateHoverActions(true, immediate);
    } else if (immediate) {
        if (hoverActionsHideTimer_) {
            hoverActionsHideTimer_->stop();
        }
        animateHoverActions(false, true);
    } else if (hoverActionsHideTimer_) {
        // Let the pointer cross from the row into the floating action surface.
        hoverActionsHideTimer_->start();
    } else {
        animateHoverActions(false);
    }

    if (continuationTime_) {
        continuationTime_->setVisible(
            hovered_ && authorRunContinuation_ && !wholeMessageSelectionMode_);
        if (continuationTime_->isVisible()) {
            continuationTime_->raise();
        }
    }
}

void PostWidget::setAuthorRunContinuation(bool continuation)
{
    if (authorRunContinuation_ == continuation) {
        return;
    }

    authorRunContinuation_ = continuation;

    if (pendingDeliveryIndicator_) {
        // A continuation has no author header, but the sending spinner used
        // to keep that header layout one chip-height tall. On confirmation
        // the authoritative continuation has no spinner and text jumped up.
        // Keep the pending indicator visible as an overlay, not a layout row.
        if (continuation && !_pendingIndicatorOverlay) {
            ui->horizontalLayout->removeWidget(pendingDeliveryIndicator_);
            pendingDeliveryIndicator_->setParent(this);
            _pendingIndicatorOverlay = true;
            pendingDeliveryIndicator_->show();
        } else if (!continuation && _pendingIndicatorOverlay) {
            ui->horizontalLayout->insertWidget(
                2, pendingDeliveryIndicator_, 0, Qt::AlignVCenter);
            _pendingIndicatorOverlay = false;
            pendingDeliveryIndicator_->show();
        }
    }


    // Keep the avatar gutter width stable so body text never jumps horizontally,
    // but collapse all repeated author-header height for continuation rows.
    ui->authorAvatar->setFixedHeight(continuation ? 0 : 48);
    ui->authorName->setVisible(!continuation);
    ui->time->setVisible(!continuation);

    if (continuation) {
        ui->authorAvatar->clear();
        ui->horizontalLayout_2->setContentsMargins(
            normalRowMargins_.left(), 0,
            normalRowMargins_.right(), 2);
    } else {
        ui->horizontalLayout_2->setContentsMargins(
            normalRowMargins_.left(), normalRowMargins_.top(),
            normalRowMargins_.right(), normalRowMargins_.bottom());
        updateAuthorAvatar();
    }

    if (continuationTime_) {
        continuationTime_->setVisible(
            continuation && hovered_ && !wholeMessageSelectionMode_);
    }

    positionHoverActions();
    ui->horizontalLayout_2->invalidate();
    ui->verticalLayout->invalidate();
    updateGeometry();
    emit dimensionsChanged();
}

void PostWidget::setDayBoundary(const QDate& day)
{
    if (dayBoundaryDate_ == day)
        return;
    dayBoundaryDate_ = day;

    // Most posts are not date boundaries (and threads/collections have none).
    // Materialize this decorative surface only for an actual day transition.
    if (!dayBoundaryWidget_ && day.isValid()) {
        // Date headers share their post's virtualized row, keeping source indices,
        // post IDs, paging, unread cursors and navigation anchors unchanged.
        dayBoundaryWidget_ = new QWidget(this);
        dayBoundaryWidget_->setObjectName(QStringLiteral("postDayBoundary"));
        auto* dayLayout = new QHBoxLayout(dayBoundaryWidget_);
        dayLayout->setContentsMargins(12, 8, 12, 4);
        dayLayout->setSpacing(12);
        const auto addRule = [dayLayout, this] {
            auto* rule = new QFrame(dayBoundaryWidget_);
            rule->setFrameShape(QFrame::HLine);
            rule->setFrameShadow(QFrame::Plain);
            rule->setForegroundRole(QPalette::Mid);
            rule->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            dayLayout->addWidget(rule, 1);
        };
        addRule();
        dayBoundaryLabel_ = new QLabel(dayBoundaryWidget_);
        dayBoundaryLabel_->setObjectName(QStringLiteral("postDayBoundaryLabel"));
        dayBoundaryLabel_->setAlignment(Qt::AlignCenter);
        // QPalette::Mid is a frame/shadow role, not a text color. In dark
        // themes it can resolve to black even when WindowText is light.
        dayBoundaryLabel_->setForegroundRole(QPalette::WindowText);
        dayLayout->addWidget(dayBoundaryLabel_);
        addRule();
        ui->postRootLayout->insertWidget(0, dayBoundaryWidget_);
        dayBoundaryWidget_->hide();
        dayBoundaryLabel_->setFont(chatFont_);
        updateTimestampPalette();
    }
    if (!dayBoundaryWidget_)
        return;

    dayBoundaryLabel_->setText(PostDateBoundaryPresentation::label(day));
    dayBoundaryWidget_->setVisible(day.isValid());
    ui->postRootLayout->invalidate();
    positionHoverActions();
    updateGeometry();
    emit dimensionsChanged();
}

void PostWidget::setShowFullTimestamp(bool enabled)
{
    if (fullTimestampVisible_ == enabled)
        return;
    fullTimestampVisible_ = enabled;
    // Long regional dates must remain readable even in a narrow collection
    // pane; allow the timestamp label to wrap instead of clipping its date.
    ui->time->setWordWrap(enabled);
    updateTimestampPresentation();
    if (timestampRefreshTimer_) {
        if (usesRelativeTimestamp() && !fullTimestampVisible_) {
            timestampRefreshTimer_->start(
                PostTimestampPresentation::threadRefreshIntervalMs(
                    static_cast<qint64>(post.create_at)));
        } else {
            timestampRefreshTimer_->stop();
        }
    }
    ui->horizontalLayout->invalidate();
    updateGeometry();
    emit dimensionsChanged();
}

void PostWidget::clearTextSelection()
{
    if (messageContent) {
        messageContent->clearSelection();
    }
    if (ui && ui->authorName && ui->authorName->selectionStart() >= 0) {
        ui->authorName->setSelection(0, 0);
    }
}

void PostWidget::animateHoverActions(bool visible, bool immediate)
{
    visible = visible
        && presentationMode_ == PresentationMode::Interactive
        && !wholeMessageSelectionMode_ && !post.isDeleted;
    hoverActionsWanted_ = visible;

    if (continuationTime_) {
        continuationTime_->setVisible(visible && authorRunContinuation_);
        if (continuationTime_->isVisible()) {
            continuationTime_->raise();
        }
    }

    if (auto* list = postListWidget()) {
        list->showPostHoverActions(*this, visible, immediate);
        return;
    }
    if (!hoverActions_ || !hoverActionsOpacity_ || !hoverActionsAnimation_) {
        return;
    }

    hoverActionsAnimation_->stop();
    if (visible) {
        positionHoverActions();
        hoverActions_->show();
        hoverActions_->raise();
    }

    if (immediate) {
        hoverActionsOpacity_->setOpacity(visible ? 1.0 : 0.0);
        if (!visible) {
            hoverActions_->hide();
        }
        return;
    }

    hoverActionsAnimation_->setStartValue(hoverActionsOpacity_->opacity());
    hoverActionsAnimation_->setEndValue(visible ? 1.0 : 0.0);
    hoverActionsAnimation_->start();
}

bool PostWidget::usesRelativeTimestamp() const
{
    return parentChatArea && parentChatArea->isThread
        && MLOptions::instance()->value<bool>(
            THREAD_RELATIVE_TIMESTAMPS, THREAD_RELATIVE_TIMESTAMPS_DEFAULT);
}

void PostWidget::updateTimestampPresentation()
{
    const qint64 timestamp = static_cast<qint64>(post.create_at);
    const QLocale locale = QLocale::system();
    const QString fullTimestamp =
        PostTimestampPresentation::fullTimestamp(timestamp, locale);
    const QString visibleTime = fullTimestampVisible_ ? fullTimestamp
        : usesRelativeTimestamp()
            ? PostTimestampPresentation::threadRelativeTime(timestamp,
                QDateTime::currentMSecsSinceEpoch(), locale)
            : PostTimestampPresentation::absoluteTime(timestamp, locale);

    ui->time->setText(visibleTime);
    ui->time->setToolTip(fullTimestamp);
    ui->time->adjustSize();

    if (continuationTime_) {
        // Narrow continuation timestamps live in the fixed avatar gutter. Keep
        // them as a regional clock time even when the full thread header uses
        // a longer relative label, otherwise "22 hours ago" would overlap body
        // text instead of behaving like Mattermost's narrow timestamp.
        continuationTime_->setText(
            PostTimestampPresentation::absoluteTime(timestamp, locale));
        continuationTime_->setToolTip(fullTimestamp);
        continuationTime_->setFixedWidth(48);
    }
}

void PostWidget::updateTimestampPalette()
{
    QColor muted = palette().color(QPalette::WindowText);
    muted.setAlphaF(0.73f);

    QPalette timePalette = ui->time->palette();
    timePalette.setColor(QPalette::WindowText, muted);
    timePalette.setColor(QPalette::Text, muted);
    ui->time->setPalette(timePalette);

    if (continuationTime_) {
        QColor continuationMuted = palette().color(QPalette::WindowText);
        continuationMuted.setAlphaF(0.50f);
        QPalette continuationPalette = continuationTime_->palette();
        continuationPalette.setColor(QPalette::WindowText, continuationMuted);
        continuationPalette.setColor(QPalette::Text, continuationMuted);
        continuationTime_->setPalette(continuationPalette);
    }

    if (dayBoundaryLabel_) {
        // Follow the current post/theme foreground, including live palette
        // changes. Do not use Mid/Midlight as a label foreground role.
        QPalette dividerPalette = dayBoundaryLabel_->palette();
        dividerPalette.setColor(QPalette::WindowText, muted);
        dividerPalette.setColor(QPalette::Text, muted);
        dayBoundaryLabel_->setPalette(dividerPalette);
    }
}

void PostWidget::updateHoverActionsPalette()
{
    if (auto* list = postListWidget()) {
        list->updatePostHoverActionsPalette(*this);
        return;
    }
    if (!hoverActions_) {
        return;
    }

    QColor background = palette().color(QPalette::Base);
    if (ChatLogWidget* log = chatLog()) {
        background = log->hoverHighlightSurfaceColor();
    }
    const QColor outline = palette().color(QPalette::Mid);

    QPalette toolbarPalette = hoverActions_->palette();
    toolbarPalette.setColor(QPalette::Window, background);
    toolbarPalette.setColor(QPalette::Button, background);
    hoverActions_->setPalette(toolbarPalette);

    const auto cssColor = [](const QColor& color) {
        return QStringLiteral("rgba(%1,%2,%3,%4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(color.alpha());
    };
    hoverActions_->setStyleSheet(QStringLiteral(
        "QFrame#postHoverActions {"
        " background-color: %1;"
        " border: 1px solid %2;"
        " border-radius: 5px;"
        "}")
        .arg(cssColor(background), cssColor(outline)));

    // Standalone posts keep their own action frame. Unlike a newly created
    // frame, its pre-tinted icons also need refreshing after a theme change.
    if (reactionAffordance_) {
        reactionAffordance_->setIcon(IconUtils::symbolicIcon(QStringLiteral(":/icons/emoji")));
    }
    if (threadAffordance_) {
        threadAffordance_->setIcon(IconUtils::symbolicIcon(QStringLiteral(":/icons/message-balloon")));
    }
    if (saveAffordance_) {
        saveAffordance_->setIcon(IconUtils::symbolicIcon(QStringLiteral(":/icons/bookmark")));
    }
    if (moreAffordance_) {
        moreAffordance_->setIcon(IconUtils::symbolicIcon(QStringLiteral(":/icons/more-horizontal")));
    }
}

void PostWidget::positionHoverActions()
{
    // The date header is a presentation adornment, not part of the post
    // itself. Anchor floating actions to the actual message, below the date.
    const int contentTop = dayBoundaryWidget_ && !dayBoundaryWidget_->isHidden()
        ? dayBoundaryWidget_->sizeHint().height() : 0;
    if (continuationTime_) {
        continuationTime_->move(normalRowMargins_.left(), contentTop);
    }

    if (_pendingIndicatorOverlay && pendingDeliveryIndicator_) {
        // Overlay geometry does not participate in the continuation's
        // sizeHint; its status and animation remain visible during delivery.
        pendingDeliveryIndicator_->move(
            std::max(0, width() - normalRowMargins_.right()
                            - pendingDeliveryIndicator_->width()),
            contentTop);
        pendingDeliveryIndicator_->raise();
    }

    if (auto* list = postListWidget()) {
        list->positionPostHoverActions(*this);
        return;
    }
    if (!hoverActions_ || !hoverActions_->parentWidget()) {
        return;
    }

    hoverActions_->adjustSize();
    QWidget* host = hoverActions_->parentWidget();
    const QPoint rowTopLeft = mapTo(host, QPoint(0, 0));
    const int maxX = std::max(0, host->width() - hoverActions_->width() - 4);
    const int maxY = std::max(0, host->height() - hoverActions_->height());

    const int x = std::max(
        0, std::min(rowTopLeft.x() + width() - hoverActions_->width() - 8,
                    maxX));
    const int y = std::max(
        0, std::min(rowTopLeft.y() + contentTop - hoverActions_->height() / 2, maxY));
    hoverActions_->move(x, y);
}

void PostWidget::showPostContextMenu(const QPoint& globalPos)
{
    if (post.isDeleted) {
        return;
    }

    setProperty("_mmqt_contextMenuActive", true);
    update();

    QMenu menu(this);
    const auto icon = [](const QString& path) { return IconUtils::symbolicIcon(path); };
    // PostContextMenuRouter delivers every descendant's ContextMenu here, so
    // image targets are resolved from the original click point.
    QWidget* hit = childAt(mapFromGlobal(globalPos));
    AttachedImageFile* attachedImage = nullptr;
    for (QWidget* current = hit; current && current != this;
         current = current->parentWidget()) {
        if ((attachedImage = qobject_cast<AttachedImageFile*>(current))) {
            break;
        }
    }
    const MessageContentWidget::InlineImage inlineImage =
        !attachedImage && messageContent && hit && messageContent->isAncestorOf(hit)
            ? messageContent->inlineImageAt(globalPos)
            : MessageContentWidget::InlineImage {};
    if (attachedImage) {
        QPointer<AttachedImageFile> target(attachedImage);
        menu.addAction(icon(QStringLiteral(":/icons/copy")), tr("Copy image"),
                       this, [this, target] {
            if (target) {
                AttachedImageFile::copyFileImageToClipboard(
                    backend_, target->imageFileId(), target->displayedImage());
            }
        });
        menu.addAction(tr("Save image as..."),
                       this, [target] {
            if (target) {
                target->saveImageAs();
            }
        });
        menu.addSeparator();
    } else if (!inlineImage.isNull()) {
        menu.addAction(icon(QStringLiteral(":/icons/copy")), tr("Copy image"),
                       this, [this, inlineImage] {
            AttachedImageFile::copyFileImageToClipboard(
                backend_, inlineImage.fileId, inlineImage.rendered);
        });
        menu.addSeparator();
    }

    // One directory chooser for every file in the post, including images
    // rendered inline in its Markdown body (not only visible attachment cards).
    if (post.files.size() >= 2) {
        menu.addAction(
            icon(QStringLiteral(":/icons/menu-file")),
            tr("Save all attachments to…"),
            this, [this] {
                AttachmentBatchSave::chooseFolderAndSave(
                    backend_, post.files, this);
            });
        menu.addSeparator();
    }

    if (presentationMode_ != PresentationMode::Interactive) {
        if (!hoveredLink.isEmpty()) {
            QAction* copyLinkAction = menu.addAction(
                icon(QStringLiteral(":/icons/link")), tr("Copy link to clipboard"));
            connect(copyLinkAction, &QAction::triggered, this, [this] {
                QApplication::clipboard()->setText(hoveredLink);
            });
        }

        const QString selectedText = getSelectedText();
        if (!selectedText.isEmpty()) {
            QAction* copySelectedAction = menu.addAction(
                icon(QStringLiteral(":/icons/copy")), tr("Copy selected text"));
            connect(copySelectedAction, &QAction::triggered, this, [selectedText] {
                QApplication::clipboard()->setText(selectedText);
            });
        }

        QAction* copyMessageAction = menu.addAction(
            icon(QStringLiteral(":/icons/copy")), tr("Copy message text"));
        connect(copyMessageAction, &QAction::triggered, this, [this] {
            QApplication::clipboard()->setText(
                formatForClipboardSelection(messageOnly));
        });

        if (presentationMode_ == PresentationMode::Pending && pendingCancel_) {
            menu.addSeparator();
            QAction* cancelAction = menu.addAction(
                icon(QStringLiteral(":/icons/trash")),
                tr("Cancel unsent message"));
            connect(cancelAction, &QAction::triggered, this, [this] {
                if (pendingCancel_) {
                    pendingCancel_();
                }
            });
        }

        if (post.author) {
            menu.addSeparator();
            QAction* profileAction = menu.addAction(
                icon(QStringLiteral(":/icons/members")),
                tr("View %1's profile").arg(post.author->getDisplayName()));
            connect(profileAction, &QAction::triggered, this, [this] {
                if (!post.author) {
                    return;
                }
                auto* dialog =
                    new UserProfileDialog(backend_, *post.author, this);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->show();
            });
        }

        menu.exec(globalPos);
        setProperty("_mmqt_contextMenuActive", false);
        update();
        return;
    }

    if (parentChatArea) {
        QAction* replyAction = menu.addAction(icon(QStringLiteral(":/icons/message-balloon")),
                                             tr("Reply"));
        connect(replyAction, &QAction::triggered, this, [this] {
            if (parentChatArea) {
                QuotedReplyController::instance(*parentChatArea).begin(post);
            }
        });
        menu.addSeparator();
    }

    if (post.isOwnPost()) {
        if (parentChatArea) {
            QAction* editAction = menu.addAction(icon(QStringLiteral(":/icons/edit")), tr("Edit"));
            connect(editAction, &QAction::triggered, this, [this] {
                parentChatArea->editPost(post);
            });
        }
        QAction* deleteAction = menu.addAction(icon(QStringLiteral(":/icons/trash")), tr("Delete"));
        connect(deleteAction, &QAction::triggered, this, [this] {
            backend_.deletePost(post.id);
        });
        menu.addSeparator();
    }

    if (!hoveredLink.isEmpty()) {
        QAction* copyLinkAction = menu.addAction(icon(QStringLiteral(":/icons/link")),
                                                tr("Copy link to clipboard"));
        connect(copyLinkAction, &QAction::triggered, this, [this] {
            QApplication::clipboard()->setText(hoveredLink);
        });
    }

    QAction* copyMessageLinkAction = menu.addAction(icon(QStringLiteral(":/icons/link")),
                                                    tr("Copy message link"));
    connect(copyMessageLinkAction, &QAction::triggered, this, [this] {
        const QString link = messagePermalink(post);
        if (!link.isEmpty()) {
            QApplication::clipboard()->setText(link);
        }
    });

    const QString selectedText = getSelectedText();
    if (!selectedText.isEmpty()) {
        QAction* copySelectedAction = menu.addAction(icon(QStringLiteral(":/icons/copy")),
                                                     tr("Copy selected text"));
        connect(copySelectedAction, &QAction::triggered, this, [selectedText] {
            QApplication::clipboard()->setText(selectedText);
        });
    }

    QAction* copyMessageAction = menu.addAction(icon(QStringLiteral(":/icons/copy")),
                                                tr("Copy post message"));
    connect(copyMessageAction, &QAction::triggered, this, [this] {
        QApplication::clipboard()->setText(formatForClipboardSelection(messageOnly));
    });

    QAction* unreadAction = menu.addAction(icon(QStringLiteral(":/icons/unread")),
                                           tr("Mark as unread"));
    connect(unreadAction, &QAction::triggered, this, [this] {
        emit markUnreadRequested(post.id);
    });

    const bool saved = backend_.isPostFlagged(post.id);
    QAction* saveAction = menu.addAction(
        icon(QStringLiteral(":/icons/bookmark")),
        saved ? tr("Remove from saved") : tr("Save message"));
    connect(saveAction, &QAction::triggered, this, [this, saved] {
        const BackendUserPreferences pref {
            QStringLiteral("flagged_post"), post.id, QStringLiteral("true")};
        if (saved) backend_.deleteUserPreferences(pref);
        else backend_.updateUserPreferences(pref);
    });

    const bool pinned = post.is_pinned;
    QAction* pinAction = menu.addAction(
        icon(QStringLiteral(":/icons/pin")),
        pinned ? tr("Unpin message") : tr("Pin message"));
    connect(pinAction, &QAction::triggered, this, [this, pinned] {
        if (pinned) {
            backend_.unpinPost(post.id, post.channel_id);
        } else {
            backend_.pinPost(post.id, post.channel_id);
        }
    });

    if (post.author) {
        menu.addSeparator();
        QAction* profileAction = menu.addAction(
            icon(QStringLiteral(":/icons/members")),
            tr("View %1's profile").arg(post.author->getDisplayName()));
        connect(profileAction, &QAction::triggered, this, [this] {
            if (!post.author) {
                return;
            }
            auto* dialog = new UserProfileDialog(backend_, *post.author, this);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->show();
        });
    }

    menu.exec(globalPos);
    setProperty("_mmqt_contextMenuActive", false);
    update();
}

void PostWidget::setEphemeralMentionNotice(const QJsonObject& ephemeralPost)
{
    if (presentationMode_ != PresentationMode::Interactive || !parentChatArea
        || post.isDeleted || !post.isOwnPost()) {
        return;
    }
    const QJsonObject details = ephemeralPost.value(QStringLiteral("props"))
        .toObject().value(QStringLiteral("add_channel_member")).toObject();
    if (ephemeralPost.value(QStringLiteral("type")).toString()
            != QStringLiteral("system_ephemeral") || details.isEmpty()) {
        return;
    }

    const QString noticeId = ephemeralPost.value(QStringLiteral("id")).toString();
    if (noticeId.isEmpty() || ephemeralMentionId_ == noticeId) {
        return;
    }
    const QJsonArray ids = details.value(
        QStringLiteral("not_in_channel_user_ids")).toArray();
    const QJsonArray usernames = details.value(
        QStringLiteral("not_in_channel_usernames")).toArray();
    const QJsonArray groupUsernames = details.value(
        QStringLiteral("not_in_groups_usernames")).toArray();
    if (ids.size() != usernames.size()
        || (usernames.isEmpty() && groupUsernames.isEmpty())) {
        return;
    }

    // Server-side group restrictions are not bypassed even when the sender
    // otherwise has manage_*_channel_members. Never offer to add these users.
    QSet<QString> restrictedIds;
    for (const QJsonValue& value : details.value(
             QStringLiteral("not_in_groups_user_ids")).toArray()) {
        restrictedIds.insert(value.toString());
    }
    QSet<QString> restrictedNames;
    for (const QJsonValue& value : groupUsernames) {
        restrictedNames.insert(value.toString());
    }

    ephemeralMentionId_ = noticeId;
    ephemeralInviteCandidates_.clear();
    QStringList mentionedNames;
    QStringList nonInvitableNames;
    for (int i = 0; i < ids.size(); ++i) {
        const QString userId = ids.at(i).toString();
        const QString username = usernames.at(i).toString();
        if (username.isEmpty()) {
            continue;
        }
        const QString display = QStringLiteral("@") + username;
        mentionedNames.append(display);
        if (restrictedIds.contains(userId) || restrictedNames.contains(username)) {
            nonInvitableNames.append(display);
        } else if (!userId.isEmpty()) {
            ephemeralInviteCandidates_.append(qMakePair(userId, username));
        }
    }
    for (const QJsonValue& value : groupUsernames) {
        const QString username = value.toString();
        if (!username.isEmpty()) {
            const QString display = QStringLiteral("@") + username;
            if (!mentionedNames.contains(display)) {
                mentionedNames.append(display);
            }
            if (!nonInvitableNames.contains(display)) {
                nonInvitableNames.append(display);
            }
        }
    }
    if (mentionedNames.isEmpty()) {
        return;
    }

    if (!ephemeralMentionRow_) {
        ephemeralMentionRow_ = new QWidget(this);
        ephemeralMentionRow_->setObjectName(QStringLiteral("ephemeralMentionNotice"));
        auto* rowLayout = new QVBoxLayout(ephemeralMentionRow_);
        rowLayout->setContentsMargins(0, 4, 0, 4);
        rowLayout->setSpacing(4);

        ephemeralMentionLabel_ = new QLabel(ephemeralMentionRow_);
        ephemeralMentionLabel_->setObjectName(QStringLiteral("ephemeralMentionText"));
        ephemeralMentionLabel_->setWordWrap(true);
        ephemeralMentionLabel_->setTextFormat(Qt::PlainText);
        ephemeralMentionLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        rowLayout->addWidget(ephemeralMentionLabel_);

        ephemeralMentionInviteButton_ = new QPushButton(
            tr("Add to channel"), ephemeralMentionRow_);
        ephemeralMentionInviteButton_->setObjectName(
            QStringLiteral("inviteMentionedUsersButton"));
        ephemeralMentionInviteButton_->setFlat(true);
        ephemeralMentionInviteButton_->setCursor(Qt::PointingHandCursor);
        connect(ephemeralMentionInviteButton_, &QPushButton::clicked,
                this, &PostWidget::inviteEphemeralMentionedUsers);

        auto* actions = new QHBoxLayout;
        actions->setContentsMargins(0, 0, 0, 0);
        actions->addWidget(ephemeralMentionInviteButton_, 0, Qt::AlignLeft);
        actions->addStretch();
        rowLayout->addLayout(actions);

        ephemeralMentionFeedback_ = new QLabel(ephemeralMentionRow_);
        ephemeralMentionFeedback_->setObjectName(
            QStringLiteral("ephemeralMentionFeedback"));
        ephemeralMentionFeedback_->setTextFormat(Qt::PlainText);
        ephemeralMentionFeedback_->setWordWrap(true);
        ephemeralMentionFeedback_->hide();
        rowLayout->addWidget(ephemeralMentionFeedback_);

        // This is visual chrome *under the original post*, not an additional
        // source row. The underlying server timeline and read cursor stay intact.
        ui->verticalLayout->addWidget(ephemeralMentionRow_);
    }

    ephemeralMentionLabel_->setText(
        tr("%1 did not get notified by your mention because they are not in this channel.")
            .arg(mentionedNames.join(QStringLiteral(", "))));
    if (!nonInvitableNames.isEmpty()) {
        ephemeralMentionLabel_->setText(
            ephemeralMentionLabel_->text() + QLatin1Char(' ')
            + tr("%1 cannot be invited because of the channel's group restrictions.")
                  .arg(nonInvitableNames.join(QStringLiteral(", "))));
    }
    if (parentChatArea->channel.membershipPolicyEnforced) {
        ephemeralMentionLabel_->setText(
            ephemeralMentionLabel_->text() + QLatin1Char(' ')
            + tr("This channel's membership policy prevents direct invitations."));
    }
    ephemeralMentionFeedback_->hide();
    ephemeralMentionInviteButton_->setText(tr("Add to channel"));
    ephemeralMentionInviteButton_->hide();
    ephemeralMentionInviteButton_->setEnabled(false);
    ephemeralMentionRow_->show();

    if (!ephemeralInviteCandidates_.isEmpty()
        && !parentChatArea->channel.membershipPolicyEnforced) {
        const QString currentId = ephemeralMentionId_;
        QPointer<PostWidget> guard(this);
        backend_.canAddChannelMembers(parentChatArea->channel,
            [guard, currentId](bool permitted) {
                if (!guard || guard->ephemeralMentionId_ != currentId
                    || !guard->ephemeralMentionInviteButton_) {
                    return;
                }
                guard->ephemeralMentionInviteButton_->setVisible(permitted);
                guard->ephemeralMentionInviteButton_->setEnabled(permitted);
                guard->ui->verticalLayout->invalidate();
                guard->updateGeometry();
                emit guard->dimensionsChanged();
            });
    }

    ui->verticalLayout->invalidate();
    updateGeometry();
    emit dimensionsChanged();
}

void PostWidget::inviteEphemeralMentionedUsers()
{
    if (!parentChatArea || !ephemeralMentionInviteButton_
        || ephemeralInviteCandidates_.isEmpty()) {
        return;
    }
    ephemeralMentionInviteButton_->setEnabled(false);
    ephemeralMentionInviteButton_->setText(tr("Adding…"));

    struct InviteBatch {
        int remaining = 0;
        QStringList failures;
    };
    auto batch = std::make_shared<InviteBatch>();
    const auto candidates = ephemeralInviteCandidates_;
    batch->remaining = candidates.size();

    QPointer<PostWidget> guard(this);
    const QString noticeId = ephemeralMentionId_;
    for (const auto& candidate : candidates) {
        const QString userId = candidate.first;
        const QString username = candidate.second;
        backend_.addChannelMember(parentChatArea->channel, userId,
            [guard, noticeId, batch, userId, username]
            (bool success, const QString& error) {
                if (!guard || guard->ephemeralMentionId_ != noticeId) {
                    return;
                }
                if (success) {
                    auto& remaining = guard->ephemeralInviteCandidates_;
                    for (int i = remaining.size() - 1; i >= 0; --i) {
                        if (remaining.at(i).first == userId) {
                            remaining.removeAt(i);
                        }
                    }
                } else {
                    batch->failures.append(
                        QStringLiteral("@%1: %2").arg(username, error));
                }

                if (--batch->remaining != 0) {
                    return;
                }
                if (guard->ephemeralInviteCandidates_.isEmpty()) {
                    guard->ephemeralMentionRow_->hide();
                    emit guard->ephemeralInviteDismissed(guard->post.create_at);
                } else {
                    guard->ephemeralMentionInviteButton_->setText(
                        guard->tr("Retry adding"));
                    guard->ephemeralMentionInviteButton_->setEnabled(true);
                    guard->ephemeralMentionFeedback_->setText(
                        batch->failures.join(QLatin1Char('\n')));
                    guard->ephemeralMentionFeedback_->show();
                }
                guard->ui->verticalLayout->invalidate();
                guard->updateGeometry();
                emit guard->dimensionsChanged();
            });
    }
}

void PostWidget::setPendingDeliveryPresentation(
    const QString& statusText,
    bool failed,
    std::function<void()> retry,
    std::function<void()> cancel)
{
    if (presentationMode_ != PresentationMode::Pending) {
        return;
    }

    pendingRetry_ = std::move(retry);
    pendingCancel_ = std::move(cancel);

    if (pendingDeliveryIndicator_) {
        pendingDeliveryIndicator_->setToolTip(statusText);
        pendingDeliveryIndicator_->setAccessibleName(statusText);
        pendingDeliveryIndicator_->setAnimating(!failed);
    }

    // Normal pending states are represented entirely inside the existing
    // author header row, so Sending/Queued/RetryWait/Blocked cannot change the
    // PostWidget height. A textual row appears only when user action is needed.
    if (!failed) {
        if (!pendingDeliveryRow_ || !pendingDeliveryRow_->isVisible()) {
            return;
        }
        pendingDeliveryRow_->hide();
        ui->verticalLayout->invalidate();
        updateGeometry();
        QTimer::singleShot(0, this, [this] {
            ui->verticalLayout->invalidate();
            ui->verticalLayout->activate();
            updateGeometry();
            emit dimensionsChanged();
        });
        return;
    }

    const bool geometryChanged =
        !pendingDeliveryRow_ || !pendingDeliveryRow_->isVisible();

    if (!pendingDeliveryRow_) {
        pendingDeliveryRow_ = new QWidget(this);
        auto* layout = new QHBoxLayout(pendingDeliveryRow_);
        layout->setContentsMargins(0, 4, 0, 0);
        layout->setSpacing(6);

        pendingDeliveryLabel_ = new QLabel(pendingDeliveryRow_);
        QPalette statusPalette = pendingDeliveryLabel_->palette();
        statusPalette.setColor(
            QPalette::WindowText,
            statusPalette.color(QPalette::Disabled, QPalette::Text));
        pendingDeliveryLabel_->setPalette(statusPalette);
        pendingDeliveryLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(pendingDeliveryLabel_);
        layout->addStretch();

        pendingRetryButton_ = new QPushButton(tr("Retry"), pendingDeliveryRow_);
        pendingRetryButton_->setFlat(true);
        pendingRetryButton_->setCursor(Qt::PointingHandCursor);
        pendingRetryButton_->setToolTip(tr("Retry sending this message"));
        connect(pendingRetryButton_, &QPushButton::clicked, this, [this] {
            if (pendingRetry_) {
                pendingRetry_();
            }
        });
        layout->addWidget(pendingRetryButton_);

        pendingCancelButton_ = new QPushButton(tr("Cancel"), pendingDeliveryRow_);
        pendingCancelButton_->setFlat(true);
        pendingCancelButton_->setCursor(Qt::PointingHandCursor);
        pendingCancelButton_->setToolTip(tr("Cancel this unsent message"));
        connect(pendingCancelButton_, &QPushButton::clicked, this, [this] {
            if (pendingCancel_) {
                pendingCancel_();
            }
        });
        layout->addWidget(pendingCancelButton_);

        ui->verticalLayout->addWidget(pendingDeliveryRow_);
    }

    pendingDeliveryLabel_->setText(statusText);
    pendingRetryButton_->setVisible(true);
    pendingCancelButton_->setVisible(static_cast<bool>(pendingCancel_));
    pendingDeliveryRow_->show();

    if (!geometryChanged) {
        return;
    }

    pendingDeliveryRow_->updateGeometry();
    ui->verticalLayout->invalidate();
    updateGeometry();
    QTimer::singleShot(0, this, [this] {
        if (!pendingDeliveryRow_) {
            return;
        }
        ui->verticalLayout->invalidate();
        ui->verticalLayout->activate();
        updateGeometry();
        emit dimensionsChanged();
    });
}

void PostWidget::setAuthor(Backend& backendInstance, const BackendUser* user)
{
	if (!user) {
		return;
	}

	post.author = user;
	ui->authorName->setText(post.getDisplayAuthorName());
    updateAuthorNameColor(ui->authorName, post.isOwnPost(), palette());

	connect(user, &BackendUser::onAvatarChanged,
	        this, &PostWidget::updateAuthorAvatar, Qt::UniqueConnection);
	connect(user, &BackendUser::onStatusChanged,
	        this, &PostWidget::updateAuthorAvatar, Qt::UniqueConnection);

	if (user->avatar.isNull()
		|| user->avatar_picture_update != user->last_picture_update) {
		UserProfileService::instance(backendInstance).ensureAvatar(*user);
	} else {
		updateAuthorAvatar();
	}

	if (user->status.isEmpty()) {
		QPointer<PostWidget> guard(this);
		UserProfileService::instance(backendInstance).ensureStatuses(
			QStringList {user->id}, [guard] {
				if (guard) {
					guard->updateAuthorAvatar();
				}
			});
	}
}

void PostWidget::updateAuthorAvatar()
{
    if (authorRunContinuation_) {
        ui->authorAvatar->clear();
        return;
    }

	if (!post.author || post.author->avatar.isNull()) {
		ui->authorAvatar->clear();
		return;
	}

	ui->authorAvatar->setPixmap(
		AvatarUtils::withStatus(post.author->avatar, 48, post.author->status, 12,
		                        palette().color(QPalette::Window)));
}

void PostWidget::setEdited(const QString& message)
{
	messageContent->setMessage(displayMessage(post, message));
	refreshPermalinkPreviews();

	if (post.poll) {
		clearMessageText();
		std::unique_ptr<PostPoll> newPoll =
			std::make_unique<PostPoll>(backend_, post, *post.poll, this);
		ui->verticalLayout->replaceWidget(poll.get(), newPoll.get());
		poll = std::move(newPoll);
	}
}

QString PostWidget::mentionTeamId() const
{
    return parentChatArea && parentChatArea->channel.team
        ? parentChatArea->channel.team->id : QString();
}

void PostWidget::refreshMentionLinks()
{
    if (!messageContent || post.poll || post.isDeleted) {
        return;
    }
    messageContent->setMessage(displayMessage(post, post.message));
}

void PostWidget::connectMessageLinks()
{
    QHash<QString, QString> groupMentionIds;
    const QString teamId = mentionTeamId();
    if (!teamId.isEmpty()) {
        groupMentionIds = MentionGroupService::instance(backend_).mentionIds(teamId);
    }

	const auto browsers = messageContent->findChildren<QTextBrowser*>();
	for (QTextBrowser* browser : browsers) {
		if (!browser) {
			continue;
		}

        QString teamName = parentChatArea && parentChatArea->channel.team
            ? parentChatArea->channel.team->name : QString();
        if (teamName.isEmpty()) {
            const QString currentTeamId = backend_.getCurrentTeamContextId();
            if (!currentTeamId.isEmpty()) {
                if (const BackendTeam* team =
                        backend_.getStorage().getTeamById(currentTeamId)) {
                    teamName = team->name;
                }
            }
        }
        UserMentionLinkifier::linkify(
            *browser->document(), groupMentionIds, teamName);

        // Both post bodies and channel topics use the same async display-name
        // resolution. Anchors keep their original team+slug destinations.
        QPointer<PostWidget> owner(this);
        UserMentionLinkifier::resolveChannelDisplayNames(
            *browser->document(), backend_, teamName, this, [owner] {
                if (owner) {
                    emit owner->dimensionsChanged();
                }
            });

		browser->setOpenLinks(false);
		browser->setOpenExternalLinks(false);
        browser->setContextMenuPolicy(Qt::CustomContextMenu);
        browser->viewport()->installEventFilter(this);
        QObject::disconnect(browser, nullptr, this, nullptr);
		connect(browser, &QTextBrowser::anchorClicked, this,
		        [this](const QUrl& url) {
            if (url.scheme() == QStringLiteral("mattermost-user")) {
                const QString username = internalLinkValue(url);
                if (!username.isEmpty()) {
                    openUserProfile(username);
                }
                return;
            }
            if (url.scheme() == QStringLiteral("mattermost-group")) {
                const QString groupId = internalLinkValue(url);
                if (!groupId.isEmpty()) {
                    openGroupMention(groupId);
                }
                return;
            }
			AppNavigationService::instance(backend_).openUrl(url);
		});

	}

    const auto codeEditors = messageContent->findChildren<QPlainTextEdit*>();
    for (QPlainTextEdit* editor : codeEditors) {
        if (!editor) {
            continue;
        }
        editor->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::disconnect(editor, nullptr, this, nullptr);
        connect(editor, &QWidget::customContextMenuRequested, this,
                [this, editor](const QPoint& pos) {
            showPostContextMenu(editor->viewport()->mapToGlobal(pos));
        });
    }
}

void PostWidget::openUserProfile(const QString& username)
{
    QPointer<PostWidget> guard(this);
    UserProfileService::instance(backend_).ensureUserByUsername(
        username, [guard](const BackendUser* user) {
            if (!guard || !user) {
                return;
            }
            UserProfileDialog::showTransient(guard->backend_, *user, guard);
        });
}

void PostWidget::applyChatFont(const QString& serializedFont,
                               bool notifyGeometry)
{
    QFont nextFont;
    if (serializedFont.isEmpty() || !nextFont.fromString(serializedFont)) {
        nextFont = QApplication::font();
    }

    chatFont_ = nextFont;
    if (dayBoundaryLabel_)
        dayBoundaryLabel_->setFont(chatFont_);

    // The designer's legacy 8 pt / 16 px author label truncated larger chat
    // fonts. The nickname uses the same body size, only with bold weight.
    QFont authorFont = chatFont_;
    authorFont.setBold(true);
    ui->authorName->setMaximumHeight(QWIDGETSIZE_MAX);
    ui->authorName->setFont(authorFont);

    // Timestamp belongs to author chrome, not message-body typography.
    // Both the normal post time and continuation hover time use the same
    // compact font relative to the author header.
    QFont headerTimeFont = ui->authorName->font();
    headerTimeFont.setBold(false);
    if (headerTimeFont.pointSizeF() > 0) {
        headerTimeFont.setPointSizeF(
            std::max(1.0, headerTimeFont.pointSizeF() * 0.90));
    }
    ui->time->setMaximumHeight(QWIDGETSIZE_MAX);
    ui->time->setFont(headerTimeFont);
    ui->horizontalLayout->setSpacing(
        qMax(2, qRound(QFontMetrics(ui->authorName->font()).height() * 0.30)));

    if (continuationTime_) {
        continuationTime_->setFont(ui->time->font());
    }
    updateTimestampPresentation();
    updateTimestampPalette();

    if (threadSummary) {
        threadSummary->setFont(chatFont_);
    }
    if (pendingDeliveryIndicator_) {
        pendingDeliveryIndicator_->setFixedHeight(
            ReactionChipStyle::chipHeight(chatFont_));
    }
    if (reactions) {
        reactions->setFont(chatFont_);
    }
    if (attachments) {
        attachments->setFont(chatFont_);
    }
    updateEngagementRowMetrics();

    if (notifyGeometry) {
        QTimer::singleShot(0, this, [this] {
            updateGeometry();
            emit dimensionsChanged();
        });
    }
}

void PostWidget::openGroupMention(const QString& groupId)
{
    const QString teamId = mentionTeamId();
    if (teamId.isEmpty()) {
        return;
    }

    auto& service = MentionGroupService::instance(backend_);
    const MentionGroup* group = service.groupById(teamId, groupId);
    const QString title = group && !group->displayName.isEmpty()
        ? group->displayName : QStringLiteral("@") + (group ? group->name : QString());

    QPointer<PostWidget> guard(this);
    service.retrieveMembers(groupId,
        [guard, title](QVector<MentionGroupMember> members) {
            if (!guard) {
                return;
            }

            auto* menu = new QMenu(guard);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            if (!title.isEmpty()) {
                QAction* titleAction = menu->addAction(title);
                titleAction->setEnabled(false);
                menu->addSeparator();
            }

            if (members.isEmpty()) {
                QAction* emptyAction = menu->addAction(guard->tr("No members"));
                emptyAction->setEnabled(false);
            } else {
                for (const MentionGroupMember& member : members) {
                    QString label = member.displayName;
                    if (!member.username.isEmpty()
                        && member.displayName.compare(member.username, Qt::CaseInsensitive) != 0) {
                        label += QStringLiteral(" (@") + member.username + QLatin1Char(')');
                    }
                    QAction* action = menu->addAction(label);
                    const QString username = member.username;
                    QObject::connect(action, &QAction::triggered, guard,
                                     [guard, username] {
                        if (guard && !username.isEmpty()) {
                            guard->openUserProfile(username);
                        }
                    });
                }
            }
            menu->popup(QCursor::pos());
        });
}

void PostWidget::ensureEngagementRow()
{
    if (engagementRow_) {
        return;
    }

    engagementRow_ = new QWidget(this);
    engagementRow_->setObjectName(QStringLiteral("postEngagementRow"));
    engagementRow_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);

    engagementLayout_ = new QHBoxLayout(engagementRow_);
    engagementLayout_->setContentsMargins(0, 0, 0, 0);
    engagementLayout_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    updateEngagementRowMetrics();

    ui->verticalLayout->addWidget(engagementRow_, 0, Qt::AlignLeft);
}

void PostWidget::updateEngagementRowMetrics()
{
    if (!engagementLayout_) {
        return;
    }

    // Keep the separation proportional to the chat typography instead of using
    // a device-pixel constant. 1.5 em leaves the two affordance groups visually
    // distinct without turning them back into separate rows.
    const int gap = qMax(
        1, qRound(QFontMetrics(chatFont_).height() * 1.5));
    engagementLayout_->setSpacing(gap);
}

void PostWidget::updateEngagementRow()
{
    if (!engagementRow_) {
        return;
    }

    const bool hasThread = threadSummary != nullptr;
    const bool hasReactions = reactions != nullptr;
    engagementRow_->setVisible(hasThread || hasReactions);
    // Thread summaries and reaction chips share the same engagement row.
    // Keep a 6px gap above either affordance, including thread-only posts.
    engagementLayout_->setContentsMargins(
        0, (hasThread || hasReactions) ? 6 : 0, 0, 0);

    updateEngagementRowMetrics();
    engagementLayout_->invalidate();
    ui->verticalLayout->invalidate();
    updateGeometry();
}

void PostWidget::createReactionList()
{
    if (post.isDeleted || post.reactions.empty()) {
        return;
    }

    reactions = std::make_unique<PostReactionList>(backend_, this);
    if (!chatFont_.family().isEmpty()) {
        reactions->setFont(chatFont_);
    }

    for (const auto& [emojiName, users] : post.reactions) {
        const auto presentation = backend_.emojiRegistry().resolveByName(emojiName);
        if (!presentation) {
            // Keep the exact wire identity visible while CustomEmojiService
            // resolves only its presentation. resolveByName() above schedules
            // that lazy lookup for valid custom names.
            reactions->addReaction(
                emojiName,
                QStringLiteral(":") + emojiName + QLatin1Char(':'),
                users);
            continue;
        }

        reactions->addReaction(
            emojiName, presentation->unicodeString, users);
    }

    connectReactionActions();
    ensureEngagementRow();
    // ThreadSummaryWidget, when present, is always the first item. A reaction
    // rebuild only appends/replaces the second item.
    engagementLayout_->addWidget(reactions.get(), 0, Qt::AlignVCenter);
    updateEngagementRow();
}

void PostWidget::updateReactions()
{
	if (reactions) {
        if (engagementLayout_) {
            engagementLayout_->removeWidget(reactions.get());
        }
		reactions.reset();
	}

	createReactionList();
    updateEngagementRow();

    // PostReactionList computes its own metrics while chips are added, before it
    // is connected to this widget and before it joins the parent layout. Commit
    // the parent layout only after the row has actually been inserted/removed,
    // then notify LongListWidget on the next event-loop turn using the settled
    // PostWidget sizeHint. This applies equally to Unicode and custom/GIF emoji.
    ui->verticalLayout->invalidate();
    ui->verticalLayout->activate();
    updateGeometry();

    QPointer<PostWidget> guard(this);
    QTimer::singleShot(0, this, [guard] {
        if (!guard) {
            return;
        }
        guard->ui->verticalLayout->invalidate();
        guard->ui->verticalLayout->activate();
        guard->updateGeometry();

        // PostWidget owns presentation/size hints, not its top-level rect.
        // LongListWidget is the single owner of physical row geometry and will
        // commit the settled sizeHint atomically when this signal is delivered.
        emit guard->dimensionsChanged();
    });
}

void PostWidget::connectReactionActions()
{
	if (!reactions) {
		return;
	}

	connect(reactions.get(), &PostReactionList::reactionClicked,
	        this, [this](const QString& emojiName) {
            const QString loginUserId = backend_.getLoginUser().id;
            if (post.hasReaction(loginUserId, emojiName)) {
                backend_.removePostReaction(post.id, emojiName);
            } else {
                backend_.addPostReaction(post.id, emojiName);
            }
	});
    connect(reactions.get(), &PostReactionList::dimensionsChanged,
            this, &PostWidget::dimensionsChanged);
}

void PostWidget::addThreadButton()
{
    // The floating toolbar is the action entry point even before the first
    // reply. The persistent summary represents existing thread state only.
    if (!parentChatArea || parentChatArea->isThread
        || post.isDeleted || post.reply_count <= 0) {
        if (threadSummary) {
            if (engagementLayout_) {
                engagementLayout_->removeWidget(threadSummary);
            }
            threadSummary->deleteLater();
            threadSummary = nullptr;
            updateEngagementRow();
            emit dimensionsChanged();
        }
        return;
    }

    if (!threadSummary) {
        threadSummary = new ThreadSummaryWidget(
            backend_, parentChatArea->getChannel(), post, this);
        if (!chatFont_.family().isEmpty()) {
            threadSummary->setFont(chatFont_);
        }
        connect(threadSummary, &ThreadSummaryWidget::clicked,
                this, &PostWidget::openThreadWindow);

        ensureEngagementRow();

        // Thread state is deliberately first; reactions follow after one
        // font-scaled gap. Rebuild the two-item order explicitly because either
        // side may have appeared first due to live updates.
        if (reactions) {
            engagementLayout_->removeWidget(reactions.get());
        }
        engagementLayout_->addWidget(threadSummary, 0, Qt::AlignVCenter);
        if (reactions) {
            engagementLayout_->addWidget(reactions.get(), 0, Qt::AlignVCenter);
        }
        updateEngagementRow();
        emit dimensionsChanged();
        return;
    }

    threadSummary->refresh();
    updateEngagementRow();
}

void PostWidget::openThreadWindow()
{
    if (!parentChatArea || post.id.isEmpty()) {
        return;
    }

    AppNavigationService::instance(backend_).openThread(
        parentChatArea->getChannel().id, post.id);
}

void PostWidget::markAsDeleted()
{
	post.isDeleted = true;
    // A deleted post no longer admits local ephemeral invite actions. An
    // in-flight permission reply must not recreate a button for its tombstone.
    ephemeralMentionId_.clear();
    ephemeralInviteCandidates_.clear();
    if (ephemeralMentionRow_) {
        ephemeralMentionRow_->hide();
    }
    animateHoverActions(false);
    setAuthorRunContinuation(false);
    if (threadSummary) {
        if (engagementLayout_) {
            engagementLayout_->removeWidget(threadSummary);
        }
        threadSummary->deleteLater();
        threadSummary = nullptr;
    }
    if (reactions && engagementLayout_) {
        engagementLayout_->removeWidget(reactions.get());
    }
    updateEngagementRow();
	quoteFrame.reset();
	quotedReplyPreview.reset();
	permalinkPreviews.clear();
	attachments.reset();
	reactions.reset();
    updateEngagementRow();
    ktalkMeeting_.reset();
	if (poll) {
		ui->verticalLayout->removeWidget(poll.get());
		poll.reset();
		messageContent->setMessage(QStringLiteral("(Poll deleted)"));
	} else {
		messageContent->setMessage(QStringLiteral("(Message deleted)"));
	}
	emit dimensionsChanged();
}

QString PostWidget::getSelectedText()
{
	return messageContent->selectedText();
}

QString PostWidget::formatMessageText(const QString& str) const
{
	return MessageFormatter::formatMessageText(str, &backend_.emojiRegistry());
}

QString PostWidget::getMessageTimeString(uint64_t timestamp) const
{
    return PostTimestampPresentation::fullTimestamp(
        static_cast<qint64>(timestamp), QLocale::system());
}

QString PostWidget::formatForClipboardSelection(FormatType formatType) const
{
	const QString visibleMessage = displayMessage(post, post.message);
	if (formatType == messageOnly) {
		return visibleMessage;
	}

	QString ret(post.getDisplayAuthorName() + "\t[" +
                getMessageTimeString(post.create_at) + "]\n");
	ret += " " + visibleMessage + "\n\n";
	return ret;
}

void PostWidget::clearMessageText()
{
	messageContent->clear();
}

} /* namespace Mattermost */
