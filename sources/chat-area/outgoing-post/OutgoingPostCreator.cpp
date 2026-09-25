/**
 * @file OutgoingPostCreator.cpp
 * @brief
 * @author Lyubomir Filipov
 * @date Feb 20, 2022
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

#include "OutgoingPostCreator.h"

#include <QColor>
#include <QDateTime>
#include <QDebug>
#include <QDragMoveEvent>
#include <QDynamicPropertyChangeEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPalette>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSizePolicy>
#include <QTextCursor>
#include <QTimer>

#include "NewPollDialog.h"
#include "OutgoingAttachmentList.h"
#include "backend/Backend.h"
#include "backend/AttachmentUploadService.h"
#include "backend/DraftService.h"
#include "backend/PostCreateService.h"
#include "backend/PendingPostService.h"
#include "backend/PostProps.h"
#include "backend/PostRepository.h"
#include "backend/UploadTrace.h"
#include "backend/emoji/EmojiRegistryNotifier.h"
#include "backend/types/BackendPost.h"
#include "chat-area/ChatLogWidget.h"
#include "chat-area/QuotedReplyFormat.h"
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"
#include "reactions/ReactionUsageTracker.h"
#include "ui/RankedEmojiPresentation.h"
#include "ui/ThemeIconWidgets.h"

namespace Mattermost {
namespace {

constexpr char EditingPostProperty[] = "_mmqt_editing_post";
constexpr int RankedEmojiCapacity = 16;
constexpr int RankedEmojiColumns = 8;
constexpr int RankedEmojiButtonExtent = 30;
constexpr int RankedEmojiPopupGap = 5;
constexpr int RankedEmojiPopupMargin = 4;
constexpr qreal RankedEmojiPopupRadius = 7.0;

class RankedEmojiPopupFrame final : public QFrame
{
public:
    explicit RankedEmojiPopupFrame(QWidget* parent)
        : QFrame(parent)
    {
        setFrameShape(QFrame::NoFrame);
        setAutoFillBackground(false);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(palette().color(QPalette::Mid), 1.0));
        painter.setBrush(palette().brush(QPalette::Base));

        const QRectF frameRect =
            QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.drawRoundedRect(
            frameRect, RankedEmojiPopupRadius, RankedEmojiPopupRadius);
    }
};

}

struct OutgoingPostData {
	const BackendPost* postToEdit = nullptr;
	std::unique_ptr<BackendNewPollData> pollData;
	QString message;
	QString replyToPostId;
	QString pendingPostId;
    QString rootId;
	QList<QString> attachmentPaths;
    QList<QString> attachmentUploadIds;
	QList<QString> attachmentIds;
    qsizetype pendingUploadCount = 0;
    QString uploadFailureText;
};

OutgoingPostCreator::OutgoingPostCreator(QWidget* parent)
    : MessageTextEditWidget(parent)
    , postToEdit(nullptr)
    , attachmentList(nullptr)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumWidth(100);
    setContextMenuPolicy(Qt::NoContextMenu);
    setAcceptRichText(false);
    setPlaceholderText(tr("Write a message"));

    draftSaveTimer = new QTimer(this);
    draftSaveTimer->setSingleShot(true);
    draftSaveTimer->setInterval(600);
    connect(draftSaveTimer, &QTimer::timeout,
            this, &OutgoingPostCreator::savePersistentDraftNow);

    rankedEmojiHideTimer = new QTimer(this);
    rankedEmojiHideTimer->setSingleShot(true);
    rankedEmojiHideTimer->setInterval(180);
    connect(rankedEmojiHideTimer, &QTimer::timeout,
            this, &OutgoingPostCreator::hideRankedEmojiPopup);

    connect(&EmojiRegistryNotifier::instance(),
            &EmojiRegistryNotifier::customEmojiAdded,
            this,
            [this](const QString&) {
                if (rankedEmojiPopup) {
                    showRankedEmojiPopup();
                }
            });
}

void OutgoingPostCreator::init(Backend& backendInstance,
                               BackendChannel& channelInstance,
                               ChatLogWidget& chatLogWidgetInstance,
                               QBoxLayout* attachmentParentLayout,
                               QLabel& statusLabelInstance,
                               QPushButton& attachButtonInstance,
                               QPushButton& addEmojiButtonInstance,
                               QPushButton& sendButtonInstance)
{
	backend = &backendInstance;
	channel = &channelInstance;
	chatLogWidget = &chatLogWidgetInstance;

    auto& attachmentUploads =
        AttachmentUploadService::instance(backendInstance);
    connect(&attachmentUploads,
            &AttachmentUploadService::changed,
            this,
            [this](const QString& uploadId) {
        if (!attachmentList) {
            return;
        }
        for (const OutgoingAttachmentItem& item : attachmentList->attachments()) {
            if (item.id == uploadId) {
                updateSendButtonState();
                return;
            }
        }
    });
	attachmentParent = attachmentParentLayout;
	statusLabel = &statusLabelInstance;
	attachButton = &attachButtonInstance;
    if (addEmojiButton) {
        addEmojiButton->removeEventFilter(this);
    }
	addEmojiButton = &addEmojiButtonInstance;
    addEmojiButton->installEventFilter(this);
	sendButton = &sendButtonInstance;

	connect(this, &MessageTextEditWidget::escapePressed, this, [this] {
		if (outgoingPostData) {
			return;
		}
        if (isEditingPost()) {
            cancelPostEdit();
            return;
        }
		if (!property(PostProps::ReplyToPostId).toString().isEmpty()) {
			setProperty(PostProps::ReplyToPostId, QString());
			return;
		}
        suppressDraftPersistence = true;
		clear();
		postToEdit = nullptr;
		editResidencyLease.reset();
		setEditingVisual(false);
        suppressDraftPersistence = false;
        discardPersistentDraft();
		emit postEditFinished();
	});

	connect(this, &MessageTextEditWidget::upArrowPressed,
	        &chatLogWidgetInstance, &ChatLogWidget::editLastOwnPost);

	connect(this, &QTextEdit::textChanged,
	        this, &OutgoingPostCreator::updateSendButtonState);
    connect(this, &QTextEdit::textChanged,
            this, &OutgoingPostCreator::schedulePersistentDraftSave);

	connect(sendButton, &QPushButton::clicked,
	        this, &OutgoingPostCreator::sendPostButtonAction);
	connect(this, &MessageTextEditWidget::enterPressed,
	        this, &OutgoingPostCreator::sendPostButtonAction);
	// ChatArea may turn the paperclip into a menu button. Keep the legacy
	// direct-file behavior only for callers that provide a plain button.
	if (!attachButton->menu()) {
		connect(attachButton, &QPushButton::clicked,
		        this, &OutgoingPostCreator::onAttachButtonClick);
	}

	connect(addEmojiButton, &QPushButton::clicked, this, [this] {
        hideRankedEmojiPopup();
		showEmojiDialog([this](Emoji emoji) {
			insertPlainText(" :" + emoji.name + ": ");
			setFocus();
		});
	});

	setEditingVisual(false);
	updateSendButtonState();
}

OutgoingPostCreator::~OutgoingPostCreator()
{
    hideRankedEmojiPopup();
    releaseComposerAttachmentUploads();
    flushPersistentDraft();
}

void OutgoingPostCreator::setStatusLabelText(const QString& string)
{
	// Transient send/upload state belongs in the fixed attach-action slot. This
	// keeps the editor geometry stable instead of inserting variable-width text
	// to its left. Editing mode remains a persistent textual state and therefore
	// continues to use composerStatusLabel.
	if (attachButton && string.isEmpty()) {
		attachButton->setProperty(ComposerBusyTextProperty, QString());
		attachButton->setToolTip(tr("Add"));
	}

	if (attachButton && outgoingPostData && !string.isEmpty()) {
		attachButton->setProperty(ComposerBusyTextProperty, string);
		attachButton->setToolTip(string);
		return;
	}

	if (statusLabel) {
		statusLabel->setText(string);
	}
}

void OutgoingPostCreator::onAttachButtonClick()
{
	QStringList files = QFileDialog::getOpenFileNames(this, "Select File(s) to attach");
	if (files.empty()) {
		return;
	}
	createAttachmentList(files);
}

void OutgoingPostCreator::postEditInitiated(BackendPost& post)
{
	if (isCreatingPost()) {
		qDebug() << "Post edit requested while creating post";
		emit postEditFinished();
		return;
	}

    flushPersistentDraft();

	// Editing and quoted reply are mutually exclusive composer modes. Keep the
	// interoperability blockquote out of the editor; it is restored on send.
    suppressDraftPersistence = true;
	setProperty(PostProps::ReplyToPostId, QString());
	postToEdit = &post;
	editResidencyLease = PostRepository::instance(*backend).leasePost(post);
	setPlainText(QuotedReplyFormat::stripFallback(post.message));
    suppressDraftPersistence = false;
	setFocus();
	moveCursor(QTextCursor::End);
	setEditingVisual(true);
	updateSendButtonState();
}

void OutgoingPostCreator::cancelPostEdit()
{
    // Once an edit has been submitted, keep the immutable request data until
    // the HTTP transaction succeeds or the user explicitly retries it.
    if (!postToEdit || outgoingPostData) {
        return;
    }

    suppressDraftPersistence = true;
    clear();
    postToEdit = nullptr;
    editResidencyLease.reset();
    setEditingVisual(false);
    suppressDraftPersistence = false;
    emit postEditFinished();
    restorePersistentDraft();
}

void OutgoingPostCreator::setEditingVisual(bool editing)
{
	setProperty(EditingPostProperty, editing);
	if (!statusLabel) {
		return;
	}

	if (editing) {
		const QColor accent = palette().color(QPalette::Highlight);
		setPlaceholderText(tr("Editing message"));
		setStyleSheet(
			QStringLiteral("QTextEdit { border: 2px solid %1; border-radius: 4px; padding: 2px; }")
				.arg(accent.name()));
		statusLabel->setText(tr("Editing message · Esc to cancel"));
		statusLabel->setStyleSheet(QStringLiteral("font-weight: 600;"));
	} else {
		setPlaceholderText(tr("Write a message"));
		setStyleSheet(QString());
		statusLabel->setStyleSheet(QString());
		if (!outgoingPostData) {
			statusLabel->clear();
		}
	}

	updateSendButtonState();
}

bool OutgoingPostCreator::isEditingPost() const
{
	return postToEdit
		|| (outgoingPostData && outgoingPostData->postToEdit);
}

static QString getStringInsideQuotes(const QString& str, int& nextPos)
{
	const int firstQuote = str.indexOf('"', nextPos);
	if (firstQuote == -1) {
		return QString();
	}

	const int lastQuote = str.indexOf('"', firstQuote + 1);
	if (lastQuote == -1) {
		return QString();
	}

	nextPos = lastQuote + 1;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
	return QStringRef(&str, firstQuote + 1, lastQuote - firstQuote - 1).toString();
#else
	QStringView view(str);
	return view.sliced(firstQuote + 1, lastQuote - firstQuote - 1).toString();
#endif
}

void OutgoingPostCreator::sendPostButtonAction()
{
	if (outgoingPostData) {
		if (!sendFailed) {
			qCInfo(lcUploadTrace)
                << "COMPOSER_SEND_IGNORED reason=already-sending";
			return;
		}

		// Keep exactly the same request body and pending_post_id across a retry.
		// Mattermost deduplicates create requests by pending_post_id, so this is
		// safe even when the previous HTTP result was ambiguous.
		sendFailed = false;
		setReadOnly(true);
		setSendActivityText();
		updateSendButtonState();
		prepareAndSendPost();
		return;
	}

	const QString message = toPlainText();
	if (message.isEmpty() && !attachmentList) {
		return;
	}

    if (isEditingPost() && hasAttachmentUploadsInProgress()) {
        qCInfo(lcUploadTrace)
            << "COMPOSER_SEND_IGNORED reason=edit-attachment-upload-in-progress";
        updateSendButtonState();
        return;
    }

	const QString replyToPostId = property(PostProps::ReplyToPostId).toString();
	if (message.startsWith("/poll")) {
		if (!replyToPostId.isEmpty()) {
			QMessageBox::warning(this, tr("Cannot quote poll"),
			                     tr("Polls cannot be sent as quoted replies. Cancel the reply first."),
			                     QMessageBox::Ok);
			return;
		}
		if (postToEdit) {
			QMessageBox::warning(this, "Error",
			                     "Cannot replace a non-poll message with a poll. Either delete the post or add the poll as a new post",
			                     QMessageBox::Ok);
			return;
		}

		BackendNewPollData initialPollData;
		int startPos = 6;
		QString str = getStringInsideQuotes(message, startPos);
		qDebug() << "got" << str << "pos" << startPos;

		if (!str.isEmpty()) {
			initialPollData.question = str;
		}

		str = getStringInsideQuotes(message, startPos);
		while (!str.isEmpty()) {
			initialPollData.options.push_back(str);
			str = getStringInsideQuotes(message, startPos);
		}

		if (message.indexOf("--progress", startPos) != -1) {
			initialPollData.showProgress = true;
		}
		if (message.indexOf("--anonymous", startPos) != -1) {
			initialPollData.isAnonymous = true;
		}
		if (message.indexOf("--public-add-option", startPos) != -1) {
			initialPollData.allowAddOptions = true;
		}

		auto* pollDialog = new NewPollDialog(this, initialPollData);
		QPointer<NewPollDialog> pollDialogGuard(pollDialog);
		connect(pollDialog, &QDialog::accepted, this, [this, pollDialogGuard] {
			if (!pollDialogGuard) {
				return;
			}
			outgoingPostData = std::make_unique<OutgoingPostData>();
			outgoingPostData->pollData =
				std::make_unique<BackendNewPollData>(pollDialogGuard->getData());
			startSendPostSequence();
		});
		pollDialog->show();
		return;
	}

	outgoingPostData = std::make_unique<OutgoingPostData>();
	outgoingPostData->message = message;
	outgoingPostData->postToEdit = postToEdit;
    outgoingPostData->rootId = root_id;
	if (!outgoingPostData->postToEdit) {
		outgoingPostData->replyToPostId = replyToPostId;
		outgoingPostData->pendingPostId = backend->getLoginUser().id
			+ QLatin1Char(':') + QString::number(QDateTime::currentMSecsSinceEpoch());
	}
	postToEdit = nullptr;

	if (attachmentList) {
        const QList<OutgoingAttachmentItem> items = attachmentList->attachments();
        auto& uploads = AttachmentUploadService::instance(*backend);
        for (const OutgoingAttachmentItem& item : items) {
            outgoingPostData->attachmentPaths.push_back(item.path);
            outgoingPostData->attachmentUploadIds.push_back(item.id);

            const AttachmentUpload* upload = uploads.upload(item.id);
            const QString readyFileId =
                upload && upload->path == item.path
                && upload->state == AttachmentUploadState::Ready
                    ? upload->fileId
                    : QString();
            outgoingPostData->attachmentIds.push_back(readyFileId);

            qCInfo(lcUploadTrace).nospace()
                << "COMPOSER_SNAPSHOT itemId=" << item.id
                << " file=" << item.path
                << " uploadKnown=" << (upload != nullptr)
                << " uploading="
                << (upload && upload->state == AttachmentUploadState::Uploading)
                << " fileId=" << readyFileId;
        }
		attachmentList->setDisableInput(true);
	}

    qCInfo(lcUploadTrace).nospace()
        << "COMPOSER_SEND_START attachments="
        << outgoingPostData->attachmentPaths.size()
        << " rootId=" << outgoingPostData->rootId
        << " replyTo=" << outgoingPostData->replyToPostId;

	startSendPostSequence();
}

void OutgoingPostCreator::startSendPostSequence()
{
	sendFailed = false;

    // Ordinary messages become outbox operations immediately. PendingPostService
    // owns any still-running attachment uploads and the eventual post create,
    // so the editor can be cleared without waiting for upload completion.
    if (outgoingPostData && !outgoingPostData->postToEdit
        && !outgoingPostData->pollData) {
        sendPost();
        return;
    }

	setReadOnly(true);
	updateSendButtonState();
	setSendActivityText();
	prepareAndSendPost();
}

void OutgoingPostCreator::setSendActivityText()
{
	if (!outgoingPostData) {
		return;
	}

	QString activityText;
    qsizetype remainingUploads = 0;
    for (qsizetype i = 0; i < outgoingPostData->attachmentPaths.size(); ++i) {
        if (i >= outgoingPostData->attachmentIds.size()
            || outgoingPostData->attachmentIds.at(i).isEmpty()) {
            ++remainingUploads;
        }
    }

	if (remainingUploads > 0) {
		activityText = remainingUploads == 1
			? tr("Uploading attachment…") : tr("Uploading attachments…");
	} else if (outgoingPostData->postToEdit) {
		activityText = tr("Saving edited message…");
	} else if (outgoingPostData->pollData) {
		activityText = tr("Sending poll…");
	} else {
		activityText = tr("Sending message…");
	}
	setStatusLabelText(activityText);
}

void OutgoingPostCreator::prepareAndSendPost()
{
	if (!outgoingPostData) {
		return;
	}

    while (outgoingPostData->attachmentIds.size()
           < outgoingPostData->attachmentPaths.size()) {
        outgoingPostData->attachmentIds.append(QString());
    }
    while (outgoingPostData->attachmentIds.size()
           > outgoingPostData->attachmentPaths.size()) {
        outgoingPostData->attachmentIds.removeLast();
    }

    QVector<qsizetype> missingIndexes;
    missingIndexes.reserve(outgoingPostData->attachmentPaths.size());
    for (qsizetype i = 0; i < outgoingPostData->attachmentPaths.size(); ++i) {
        if (outgoingPostData->attachmentIds.at(i).isEmpty()) {
            missingIndexes.push_back(i);
        }
    }

    if (missingIndexes.isEmpty()) {
        qCInfo(lcUploadTrace).nospace()
            << "COMPOSER_UPLOADS_READY count="
            << outgoingPostData->attachmentIds.size();
        setSendActivityText();
        sendPost();
        return;
    }

    qCInfo(lcUploadTrace).nospace()
        << "COMPOSER_UPLOAD_RETRY_BATCH missing=" << missingIndexes.size()
        << " total=" << outgoingPostData->attachmentPaths.size();

    outgoingPostData->pendingUploadCount = missingIndexes.size();
    outgoingPostData->uploadFailureText.clear();

    QPointer<OutgoingPostCreator> guard(this);
    for (const qsizetype index : missingIndexes) {
        const QString filePath = outgoingPostData->attachmentPaths.at(index);
        qCInfo(lcUploadTrace).nospace()
            << "COMPOSER_UPLOAD_RETRY index=" << index
            << " file=" << filePath;
        backend->uploadFile(
            *channel,
            filePath,
            [guard, index](QString fileId, QString errorText) {
                if (!guard || !guard->outgoingPostData
                    || index < 0
                    || index >= guard->outgoingPostData->attachmentIds.size()) {
                    return;
                }

                qCInfo(lcUploadTrace).nospace()
                    << "COMPOSER_UPLOAD_RETRY_RESULT index=" << index
                    << " fileId=" << fileId
                    << " error=" << errorText;

                if (fileId.isEmpty()) {
                    if (guard->outgoingPostData->uploadFailureText.isEmpty()) {
                        guard->outgoingPostData->uploadFailureText =
                            errorText.trimmed();
                    }
                } else {
                    guard->outgoingPostData->attachmentIds[index] = fileId;
                }

                if (guard->outgoingPostData->pendingUploadCount > 0) {
                    --guard->outgoingPostData->pendingUploadCount;
                }

                qsizetype uploadedCount = 0;
                for (const QString& id : guard->outgoingPostData->attachmentIds) {
                    if (!id.isEmpty()) {
                        ++uploadedCount;
                    }
                }

                if (guard->outgoingPostData->pendingUploadCount > 0) {
                    guard->setStatusLabelText(
                        guard->tr("Uploading attachments · %1 of %2 complete")
                            .arg(uploadedCount)
                            .arg(guard->outgoingPostData->attachmentPaths.size()));
                    return;
                }

                for (const QString& id : guard->outgoingPostData->attachmentIds) {
                    if (id.isEmpty()) {
                        QString status = guard->tr(
                            "Attachment upload failed · click Send to retry");
                        if (!guard->outgoingPostData->uploadFailureText.isEmpty()) {
                            status = guard->tr(
                                "Attachment upload failed: %1 · click Send to retry")
                                .arg(guard->outgoingPostData->uploadFailureText);
                        }
                        guard->failAttachmentUpload(status);
                        return;
                    }
                }

                guard->setSendActivityText();
                guard->sendPost();
            });
    }
}

void OutgoingPostCreator::sendPost()
{
	if (!outgoingPostData) {
		return;
	}

    qCInfo(lcUploadTrace).nospace()
        << "POST_SUBMIT attachmentPaths="
        << outgoingPostData->attachmentPaths.size()
        << " attachmentIds=" << outgoingPostData->attachmentIds
        << " rootId=" << outgoingPostData->rootId
        << " pendingPostId=" << outgoingPostData->pendingPostId;

	const QString attachmentsLogStr(outgoingPostData->attachmentIds.isEmpty()
	                                    ? "" : " (+attachments)");
	QPointer<OutgoingPostCreator> guard(this);
	auto& service = PostCreateService::instance(*backend);

	if (outgoingPostData->postToEdit) {
		qDebug() << "Send post edit" << attachmentsLogStr;
		QString wireMessage = outgoingPostData->message;
		const QString existingFallback =
			QuotedReplyFormat::fallbackPrefix(outgoingPostData->postToEdit->message);
		if (!existingFallback.isEmpty()) {
			wireMessage.prepend(existingFallback);
		}
		service.editPost(outgoingPostData->postToEdit->id,
		                 wireMessage,
		                 outgoingPostData->attachmentIds,
		                 [guard](BackendPost* post) {
			if (!guard) {
				return;
			}
			if (post) {
				guard->finishSend(post->id);
			} else {
				guard->failSend();
			}
		});
	} else if (outgoingPostData->pollData) {
		service.submitPoll(*channel, *outgoingPostData->pollData,
		                   [guard](bool success) {
			if (!guard) {
				return;
			}
			if (success) {
				guard->finishSend();
			} else {
				guard->failSend();
			}
		});
	} else {
		qDebug() << "Send post" << attachmentsLogStr;
		QJsonObject props;
		QString wireMessage = outgoingPostData->message;
		if (!outgoingPostData->replyToPostId.isEmpty()) {
			props.insert(PostProps::ReplyToPostId, outgoingPostData->replyToPostId);

			if (BackendPost* quotedPost = channel->postIdToPost.value(
			        outgoingPostData->replyToPostId, nullptr)) {
				wireMessage.prepend(QuotedReplyFormat::buildFallback(
					quotedPost->id,
					quotedPost->getDisplayAuthorName(),
					quotedPost->message,
					!quotedPost->files.empty()));
			} else {
				qWarning() << "Quoted reply target is not materialized:"
				           << outgoingPostData->replyToPostId;
			}
		}
        QList<PendingAttachment> pendingAttachments;
        const qsizetype attachmentCount =
            outgoingPostData->attachmentPaths.size();
        pendingAttachments.reserve(attachmentCount);
        for (qsizetype i = 0; i < attachmentCount; ++i) {
            PendingAttachment attachment;
            attachment.path = outgoingPostData->attachmentPaths.at(i);
            if (i < outgoingPostData->attachmentUploadIds.size()) {
                attachment.uploadId =
                    outgoingPostData->attachmentUploadIds.at(i);
            }
            if (i < outgoingPostData->attachmentIds.size()) {
                attachment.fileId =
                    outgoingPostData->attachmentIds.at(i);
            }
            pendingAttachments.push_back(std::move(attachment));
        }

        const QString pendingPostId =
            PendingPostService::instance(*backend).enqueue(
                *channel,
                wireMessage,
                pendingAttachments,
                outgoingPostData->rootId,
                props,
                outgoingPostData->pendingPostId);
        if (pendingPostId.isEmpty()) {
            failSend();
            return;
        }

        // The local outbox row is now the user's immediate acknowledgement.
        // Release the composer before any network round trip; the outbox owns
        // delivery/retry/correlation from this point on.
        finishSend(pendingPostId);
	}
}

void OutgoingPostCreator::onDragEnterEvent(QDragEnterEvent* event)
{
	if (event->mimeData()->hasUrls()) {
		event->acceptProposedAction();
	}
}

void OutgoingPostCreator::onDragMoveEvent(QDragMoveEvent* event)
{
	if (event->mimeData()->hasUrls()) {
		event->acceptProposedAction();
	}
}

void OutgoingPostCreator::onDropEvent(QDropEvent* event)
{
	if (isWaitingForPostServerResponse()) {
		qDebug() << "Cannot attach files while sending a post";
		return;
	}

	QStringList files;
	for (auto& url : event->mimeData()->urls()) {
		files.push_back(url.toLocalFile());
	}
	createAttachmentList(files);
}

void OutgoingPostCreator::onPostReceived(BackendPost& post)
{
	if (!outgoingPostData) {
		return;
	}

	if (outgoingPostData->postToEdit) {
		if (post.id != outgoingPostData->postToEdit->id) {
			return;
		}
	} else if (outgoingPostData->pollData) {
		// Matterpoll's generated post has no composer correlation token. Its HTTP
		// submit response is therefore the acknowledgement for this operation.
		return;
	} else if (outgoingPostData->pendingPostId.isEmpty()
	           || post.pending_post_id != outgoingPostData->pendingPostId) {
		return;
	}

	finishSend(post.id);
}

void OutgoingPostCreator::finishSend(const QString& confirmedPostId)
{
	if (!outgoingPostData) {
		return;
	}

	const bool wasEdit = outgoingPostData->postToEdit != nullptr;
	const bool wasPoll = outgoingPostData->pollData != nullptr;
	if (wasEdit) {
		emit postEditFinished();
	}

	qCInfo(lcUploadTrace).nospace()
        << "COMPOSER_SEND_FINISH postId=" << confirmedPostId;

	outgoingPostData.reset();
	editResidencyLease.reset();
	sendFailed = false;

    if (!wasEdit && backend && channel) {
        discardPersistentDraft();
    }

    suppressDraftPersistence = true;
	setProperty(PostProps::ReplyToPostId, QString());

	if (attachmentList) {
		delete attachmentList;
		attachmentList = nullptr;
	}

	clear();
	setReadOnly(false);
	setEditingVisual(false);
    suppressDraftPersistence = false;
	setStatusLabelText(QString());
	updateSendButtonState();

    if (wasEdit) {
        restorePersistentDraft();
    }

	if (!wasEdit && !wasPoll && !confirmedPostId.isEmpty() && chatLogWidget) {
		chatLogWidget->followOwnPost(confirmedPostId);
	}
}

void OutgoingPostCreator::failAttachmentUpload(const QString& statusText)
{
    if (!outgoingPostData) {
        return;
    }

    // Upload failure is not an ambiguous post mutation. Return to the editable
    // draft instead of trapping the composer in retry-only mode. This also lets
    // the user remove a file rejected by server policy before sending again.
    const BackendPost* editingPost = outgoingPostData->postToEdit;
    outgoingPostData.reset();
    postToEdit = editingPost;
    sendFailed = false;

    setReadOnly(false);
    if (attachmentList) {
        attachmentList->setDisableInput(false);
    }
    if (attachButton) {
        attachButton->setProperty(ComposerBusyTextProperty, QString());
        attachButton->setToolTip(tr("Add"));
    }

    setStatusLabelText(statusText);
    updateSendButtonState();
    setFocus();
}

void OutgoingPostCreator::failSend(const QString& statusText)
{
	if (!outgoingPostData) {
		return;
	}

	sendFailed = true;
	setReadOnly(true);
    if (!statusText.isEmpty()) {
        setStatusLabelText(statusText);
	} else if (outgoingPostData->postToEdit) {
		setStatusLabelText(tr("Save failed · click Send to retry"));
	} else if (outgoingPostData->pollData) {
		setStatusLabelText(tr("Poll send failed · click Send to retry"));
	} else {
		setStatusLabelText(tr("Send failed · click Send to retry"));
	}
	updateSendButtonState();
}

void OutgoingPostCreator::createAttachmentList(QStringList& files)
{
	if (!attachmentList) {
		attachmentList = new OutgoingAttachmentList(this);
		attachmentParent->insertWidget(0, attachmentList);

        connect(attachmentList,
                &OutgoingAttachmentList::fileAdded,
                this,
                [this](const QString& itemId, const QString& path) {
                    qCInfo(lcUploadTrace).nospace()
                        << "COMPOSER_ATTACHMENT_ADDED itemId=" << itemId
                        << " file=" << path;
                    startAttachmentUpload(itemId, path);
                    schedulePersistentDraftSave();
                });
        connect(attachmentList,
                &OutgoingAttachmentList::fileRemoved,
                this,
                [this](const QString& itemId, const QString& path) {
                    qCInfo(lcUploadTrace).nospace()
                        << "COMPOSER_ATTACHMENT_REMOVED itemId=" << itemId
                        << " file=" << path;
                    if (backend) {
                        AttachmentUploadService::instance(*backend).release(itemId);
                    }
                    schedulePersistentDraftSave();
                    updateSendButtonState();
                });
		connect(attachmentList, &OutgoingAttachmentList::deleted, this, [this] {
			attachmentParent->removeWidget(attachmentList);
			delete attachmentList;
			attachmentList = nullptr;
			updateSendButtonState();
		});
	}

	for (auto& filename : files) {
		attachmentList->addFile(filename);
	}
    updateSendButtonState();
}

void OutgoingPostCreator::startAttachmentUpload(const QString& itemId,
                                                const QString& path)
{
    if (!backend || !channel || itemId.isEmpty() || path.isEmpty()) {
        return;
    }

    qCInfo(lcUploadTrace).nospace()
        << "COMPOSER_UPLOAD_STAGE itemId=" << itemId
        << " channel=" << channel->id
        << " file=" << path;

    AttachmentUploadService::instance(*backend).stage(
        *channel, path, itemId);
    updateSendButtonState();
}

bool OutgoingPostCreator::hasAttachmentUploadsInProgress() const
{
    if (!backend || !attachmentList) {
        return false;
    }

    const auto& uploads = AttachmentUploadService::instance(*backend);
    for (const OutgoingAttachmentItem& item : attachmentList->attachments()) {
        const AttachmentUpload* upload = uploads.upload(item.id);
        if (upload && upload->state == AttachmentUploadState::Uploading) {
            return true;
        }
    }
    return false;
}

bool OutgoingPostCreator::hasAttachmentUploadFailures() const
{
    if (!backend || !attachmentList) {
        return false;
    }

    const auto& uploads = AttachmentUploadService::instance(*backend);
    for (const OutgoingAttachmentItem& item : attachmentList->attachments()) {
        const AttachmentUpload* upload = uploads.upload(item.id);
        if (upload && upload->state == AttachmentUploadState::Failed) {
            return true;
        }
    }
    return false;
}

void OutgoingPostCreator::releaseComposerAttachmentUploads()
{
    if (!backend || !attachmentList) {
        return;
    }

    auto& uploads = AttachmentUploadService::instance(*backend);
    for (const OutgoingAttachmentItem& item : attachmentList->attachments()) {
        uploads.release(item.id);
    }
}

void OutgoingPostCreator::updateSendButtonState()
{
	if (!sendButton || !attachButton) {
		return;
	}

	const bool editing = isEditingPost();
	sendButton->setText(editing ? QStringLiteral("✓") : QStringLiteral("➤"));
	sendButton->setAccessibleName(editing ? tr("Save edited message") : tr("Send"));

	bool sendButtonEnabled = true;
	QString tooltipText;

	if (outgoingPostData) {
		if (sendFailed) {
			if (editing) {
				tooltipText = tr("Retry saving edited message");
			} else if (outgoingPostData->pollData) {
				tooltipText = tr("Retry sending poll");
			} else {
				tooltipText = tr("Retry sending message");
			}
		} else {
			sendButtonEnabled = false;
			tooltipText = editing ? tr("Saving edited message") : tr("Waiting for server response");
		}
    } else if (isEditingPost() && hasAttachmentUploadsInProgress()) {
        sendButtonEnabled = false;
        tooltipText = tr("Uploading attachments…");
	} else if (!isCreatingPost()) {
		sendButtonEnabled = false;
		tooltipText = tr("Cannot send empty message");
    } else if (hasAttachmentUploadsInProgress()) {
        tooltipText = tr("Send · attachment upload will continue in outbox");
    } else if (hasAttachmentUploadFailures()) {
        tooltipText = tr("Send · failed attachment upload can be retried from outbox");
	} else if (editing) {
		tooltipText = tr("Save edited message · Esc to cancel");
	} else {
		tooltipText = tr("Send");
	}

	attachButton->setDisabled(outgoingPostData != nullptr);
	if (!outgoingPostData) {
		attachButton->setToolTip(tr("Add"));
	} else {
		const QString busyText = attachButton->property(ComposerBusyTextProperty).toString();
		attachButton->setToolTip(busyText.isEmpty() ? tooltipText : busyText);
	}

	sendButton->setDisabled(!sendButtonEnabled);
	sendButton->setToolTip(tooltipText);
}

bool OutgoingPostCreator::isCreatingPost()
{
	if (isWaitingForPostServerResponse()) {
		return true;
	}

	const QString message = toPlainText();
	return !message.isEmpty() || attachmentList;
}

bool OutgoingPostCreator::isWaitingForPostServerResponse()
{
	return outgoingPostData != nullptr;
}

bool OutgoingPostCreator::eventFilter(QObject* watched, QEvent* event)
{
    if (!event) {
        return MessageTextEditWidget::eventFilter(watched, event);
    }

    const QEvent::Type type = event->type();
    if (watched == rankedEmojiPopup.data()) {
        if (type == QEvent::Enter) {
            rankedEmojiHideTimer->stop();
        } else if (type == QEvent::Leave) {
            scheduleRankedEmojiPopupHide();
        } else if (type == QEvent::Destroy) {
            rankedEmojiPopup.clear();
        }
        return MessageTextEditWidget::eventFilter(watched, event);
    }

    if (watched == addEmojiButton) {
        if (type == QEvent::Enter) {
            rankedEmojiHideTimer->stop();
            showRankedEmojiPopup();
        } else if (type == QEvent::Leave) {
            scheduleRankedEmojiPopupHide();
        } else if (type == QEvent::MouseButtonPress) {
            // Pressing the affordance still opens the complete chooser through
            // its normal clicked connection.
            hideRankedEmojiPopup();
        } else if (type == QEvent::Move || type == QEvent::Resize
                   || type == QEvent::Show) {
            positionRankedEmojiPopup();
        } else if (type == QEvent::Destroy) {
            hideRankedEmojiPopup();
            addEmojiButton = nullptr;
        }
    }

    return MessageTextEditWidget::eventFilter(watched, event);
}

void OutgoingPostCreator::showRankedEmojiPopup()
{
    if (!addEmojiButton || !addEmojiButton->isVisible()) {
        hideRankedEmojiPopup();
        return;
    }

    const QStringList rankedNames =
        ReactionUsageTracker::instance().topNames(RankedEmojiCapacity);
    const QStringList names =
        RankedEmojiPresentation::renderableNames(rankedNames);
    if (names.isEmpty()) {
        hideRankedEmojiPopup();
        return;
    }

    hideRankedEmojiPopup();

    QWidget* host = addEmojiButton->window();
    if (!host) {
        return;
    }

    auto* popup = new RankedEmojiPopupFrame(host);
    rankedEmojiPopup = popup;
    popup->installEventFilter(this);

    auto* layout = new QGridLayout(popup);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setHorizontalSpacing(2);
    layout->setVerticalSpacing(2);

    int rendered = 0;
    for (const QString& name : names) {
        if (rendered >= RankedEmojiCapacity) {
            break;
        }

        auto* emojiButton = new QPushButton(popup);
        emojiButton->setFlat(true);
        emojiButton->setFixedSize(
            RankedEmojiButtonExtent, RankedEmojiButtonExtent);
        emojiButton->setCursor(Qt::PointingHandCursor);
        if (!RankedEmojiPresentation::configureButton(
                *emojiButton, name)) {
            delete emojiButton;
            continue;
        }

        emojiButton->setAccessibleName(
            tr("Insert :%1:").arg(name));
        const int row = rendered / RankedEmojiColumns;
        const int column = rendered % RankedEmojiColumns;
        layout->addWidget(emojiButton, row, column);
        ++rendered;

        connect(emojiButton, &QPushButton::clicked, popup,
                [this, name] {
            insertPlainText(QStringLiteral(" :%1: ").arg(name));
            setFocus();
            hideRankedEmojiPopup();
        });
    }

    if (rendered == 0) {
        hideRankedEmojiPopup();
        return;
    }

    popup->adjustSize();
    positionRankedEmojiPopup();
    popup->show();
    popup->raise();
}

void OutgoingPostCreator::positionRankedEmojiPopup()
{
    if (!rankedEmojiPopup || !addEmojiButton) {
        return;
    }

    QWidget* host = rankedEmojiPopup->parentWidget();
    if (!host || addEmojiButton->window() != host) {
        return;
    }

    const QPoint buttonTopLeft =
        addEmojiButton->mapTo(host, QPoint(0, 0));

    int x = buttonTopLeft.x()
        + (addEmojiButton->width() - rankedEmojiPopup->width()) / 2;
    const int maxX = qMax(
        RankedEmojiPopupMargin,
        host->width() - rankedEmojiPopup->width()
            - RankedEmojiPopupMargin);
    x = qBound(RankedEmojiPopupMargin, x, maxX);

    int y = buttonTopLeft.y()
        - rankedEmojiPopup->height() - RankedEmojiPopupGap;
    if (y < RankedEmojiPopupMargin) {
        y = buttonTopLeft.y() + addEmojiButton->height()
            + RankedEmojiPopupGap;
    }
    const int maxY = qMax(
        RankedEmojiPopupMargin,
        host->height() - rankedEmojiPopup->height()
            - RankedEmojiPopupMargin);
    y = qBound(RankedEmojiPopupMargin, y, maxY);

    rankedEmojiPopup->move(x, y);
    rankedEmojiPopup->raise();
}

void OutgoingPostCreator::scheduleRankedEmojiPopupHide()
{
    if (rankedEmojiPopup && rankedEmojiHideTimer) {
        rankedEmojiHideTimer->start();
    }
}

void OutgoingPostCreator::hideRankedEmojiPopup()
{
    if (rankedEmojiHideTimer) {
        rankedEmojiHideTimer->stop();
    }
    if (rankedEmojiPopup) {
        rankedEmojiPopup->removeEventFilter(this);
        rankedEmojiPopup->hide();
        rankedEmojiPopup->deleteLater();
        rankedEmojiPopup.clear();
    }
}

bool OutgoingPostCreator::event(QEvent* event)
{
    const bool replyPropertyChanged = event
        && event->type() == QEvent::DynamicPropertyChange
        && static_cast<QDynamicPropertyChangeEvent*>(event)->propertyName()
            == QByteArray(PostProps::ReplyToPostId);

    const bool handled = MessageTextEditWidget::event(event);
    if (replyPropertyChanged) {
        schedulePersistentDraftSave();
    }
    return handled;
}

void OutgoingPostCreator::schedulePersistentDraftSave()
{
    if (suppressDraftPersistence || !draftSaveTimer || !backend || !channel
        || isEditingPost()) {
        return;
    }
    draftSaveTimer->start();
}

void OutgoingPostCreator::savePersistentDraftNow()
{
    if (suppressDraftPersistence || !backend || !channel || isEditingPost()) {
        return;
    }

    auto& drafts = DraftService::instance(*backend);
    const QString message = toPlainText();
    const QString replyToPostId =
        property(PostProps::ReplyToPostId).toString();
    if (!activeRecoveredDraftKey.isEmpty()) {
        QStringList attachmentPaths;
        if (attachmentList) {
            for (const OutgoingAttachmentItem& item : attachmentList->attachments()) {
                attachmentPaths.push_back(item.path);
            }
        }
        drafts.updateRecoveredDraft(
            activeRecoveredDraftKey, message, replyToPostId,
            attachmentPaths);
    } else {
        drafts.updateDraft(
            channel->id, root_id, message, replyToPostId);
    }
}

void OutgoingPostCreator::flushPersistentDraft()
{
    if (!backend || !channel || isEditingPost()) {
        return;
    }

    if (draftSaveTimer && draftSaveTimer->isActive()) {
        draftSaveTimer->stop();
        savePersistentDraftNow();
    }

    if (activeRecoveredDraftKey.isEmpty()) {
        DraftService::instance(*backend).flushDraft(channel->id, root_id);
    }
}

void OutgoingPostCreator::discardPersistentDraft()
{
    if (draftSaveTimer) {
        draftSaveTimer->stop();
    }
    if (backend && channel) {
        auto& drafts = DraftService::instance(*backend);
        if (!activeRecoveredDraftKey.isEmpty()) {
            drafts.removeDraftByKey(activeRecoveredDraftKey);
            activeRecoveredDraftKey.clear();
        } else {
            drafts.removeDraft(channel->id, root_id);
        }
    }
}

void OutgoingPostCreator::restorePersistentDraft(const QString& draftKey)
{
    if (!backend || !channel || isEditingPost() || outgoingPostData) {
        return;
    }

    // Switching between the ordinary conversation draft and a recovered
    // outbox draft must not discard edits still waiting in the debounce timer.
    // flushPersistentDraft() writes through the identity that is active before
    // we replace activeRecoveredDraftKey below.
    flushPersistentDraft();

    DraftEntry draft;
    auto& drafts = DraftService::instance(*backend);
    const bool requestedRecovered = !draftKey.isEmpty();
    const bool found = requestedRecovered
        ? drafts.findDraftByKey(draftKey, draft)
        : drafts.findDraft(channel->id, root_id, draft);

    suppressDraftPersistence = true;
    releaseComposerAttachmentUploads();
    if (attachmentList) {
        attachmentParent->removeWidget(attachmentList);
        delete attachmentList;
        attachmentList = nullptr;
    }

    activeRecoveredDraftKey =
        found && draft.isRecovered() ? draft.key() : QString();
    if (found) {
        setPlainText(draft.message);
        setProperty(PostProps::ReplyToPostId, draft.replyToPostId);
        moveCursor(QTextCursor::End);
        if (draft.isRecovered() && !draft.attachmentPaths.isEmpty()) {
            QStringList paths = draft.attachmentPaths;
            createAttachmentList(paths);
        }
    } else {
        clear();
        setProperty(PostProps::ReplyToPostId, QString());
    }
    suppressDraftPersistence = false;
    updateSendButtonState();
}

void OutgoingPostCreator::setRootId(QString id)
{
	root_id = std::move(id);
}

} /* namespace Mattermost */