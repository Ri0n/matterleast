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
#include <QFileDialog>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QSizePolicy>
#include <QTextCursor>

#include "NewPollDialog.h"
#include "OutgoingAttachmentList.h"
#include "backend/Backend.h"
#include "backend/PostCreateService.h"
#include "backend/PostProps.h"
#include "backend/PostRepository.h"
#include "backend/UploadTrace.h"
#include "backend/types/BackendPost.h"
#include "chat-area/ChatLogWidget.h"
#include "chat-area/QuotedReplyFormat.h"
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"
#include "ui/ThemeIconWidgets.h"

namespace Mattermost {
namespace {

constexpr char EditingPostProperty[] = "_mmqt_editing_post";

}

struct OutgoingPostData {
	const BackendPost* postToEdit = nullptr;
	std::unique_ptr<BackendNewPollData> pollData;
	QString message;
	QString replyToPostId;
	QString pendingPostId;
    QString rootId;
	QList<QString> attachmentPaths;
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
	attachmentParent = attachmentParentLayout;
	statusLabel = &statusLabelInstance;
	attachButton = &attachButtonInstance;
	addEmojiButton = &addEmojiButtonInstance;
	sendButton = &sendButtonInstance;

	connect(this, &MessageTextEditWidget::escapePressed, this, [this] {
		if (outgoingPostData) {
			return;
		}
		if (!isEditingPost()
		    && !property(PostProps::ReplyToPostId).toString().isEmpty()) {
			setProperty(PostProps::ReplyToPostId, QString());
			return;
		}
		clear();
		postToEdit = nullptr;
		editResidencyLease.reset();
		setEditingVisual(false);
		emit postEditFinished();
	});

	connect(this, &MessageTextEditWidget::upArrowPressed,
	        &chatLogWidgetInstance, &ChatLogWidget::editLastOwnPost);

	connect(this, &QTextEdit::textChanged,
	        this, &OutgoingPostCreator::updateSendButtonState);

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
		showEmojiDialog([this](Emoji emoji) {
			insertPlainText(" :" + emoji.name + ": ");
			setFocus();
		});
	});

	setEditingVisual(false);
	updateSendButtonState();
}

OutgoingPostCreator::~OutgoingPostCreator() = default;

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

	// Editing and quoted reply are mutually exclusive composer modes. Keep the
	// interoperability blockquote out of the editor; it is restored on send.
	setProperty(PostProps::ReplyToPostId, QString());
	postToEdit = &post;
	editResidencyLease = PostRepository::instance(*backend).leasePost(post);
	setText(QuotedReplyFormat::stripFallback(post.message));
	setFocus();
	moveCursor(QTextCursor::End);
	setEditingVisual(true);
	updateSendButtonState();
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

    if (hasAttachmentUploadsInProgress()) {
        qCInfo(lcUploadTrace)
            << "COMPOSER_SEND_IGNORED reason=attachment-upload-in-progress";
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
        for (const OutgoingAttachmentItem& item : items) {
            outgoingPostData->attachmentPaths.push_back(item.path);

            const auto upload = attachmentUploads.constFind(item.id);
            const QString readyFileId =
                upload != attachmentUploads.cend()
                && upload->path == item.path
                && !upload->uploading
                    ? upload->fileId
                    : QString();
            outgoingPostData->attachmentIds.push_back(readyFileId);

            qCInfo(lcUploadTrace).nospace()
                << "COMPOSER_SNAPSHOT itemId=" << item.id
                << " file=" << item.path
                << " uploadKnown=" << (upload != attachmentUploads.cend())
                << " uploading="
                << (upload != attachmentUploads.cend() && upload->uploading)
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
		service.createPost(*channel, wireMessage, outgoingPostData->attachmentIds,
		                   outgoingPostData->rootId, props,
                       outgoingPostData->pendingPostId,
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
    attachmentUploads.clear();
	editResidencyLease.reset();
	sendFailed = false;
	setProperty(PostProps::ReplyToPostId, QString());

	if (attachmentList) {
		delete attachmentList;
		attachmentList = nullptr;
	}

	clear();
	setReadOnly(false);
	setEditingVisual(false);
	setStatusLabelText(QString());
	updateSendButtonState();

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
                });
        connect(attachmentList,
                &OutgoingAttachmentList::fileRemoved,
                this,
                [this](const QString& itemId, const QString& path) {
                    qCInfo(lcUploadTrace).nospace()
                        << "COMPOSER_ATTACHMENT_REMOVED itemId=" << itemId
                        << " file=" << path;
                    attachmentUploads.remove(itemId);
                    updateSendButtonState();
                });
		connect(attachmentList, &OutgoingAttachmentList::deleted, this, [this] {
			attachmentParent->removeWidget(attachmentList);
			delete attachmentList;
			attachmentList = nullptr;
            attachmentUploads.clear();
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

    AttachmentUploadState& state = attachmentUploads[itemId];
    state.path = path;
    state.fileId.clear();
    state.error.clear();
    state.uploading = true;
    const quint64 generation = ++state.generation;

    qCInfo(lcUploadTrace).nospace()
        << "COMPOSER_UPLOAD_START itemId=" << itemId
        << " generation=" << generation
        << " channel=" << channel->id
        << " file=" << path;

    updateSendButtonState();

    QPointer<OutgoingPostCreator> guard(this);
    backend->uploadFile(
        *channel,
        path,
        [guard, itemId, generation](QString fileId, QString errorText) {
            if (!guard) {
                return;
            }

            auto upload = guard->attachmentUploads.find(itemId);
            if (upload == guard->attachmentUploads.end()
                || upload->generation != generation) {
                qCInfo(lcUploadTrace).nospace()
                    << "COMPOSER_UPLOAD_STALE itemId=" << itemId
                    << " generation=" << generation;
                return;
            }

            upload->uploading = false;
            upload->fileId = fileId;
            upload->error = errorText.trimmed();

            if (fileId.isEmpty()) {
                qCWarning(lcUploadTrace).nospace()
                    << "COMPOSER_UPLOAD_FAILED itemId=" << itemId
                    << " generation=" << generation
                    << " file=" << upload->path
                    << " error=" << upload->error;
            } else {
                qCInfo(lcUploadTrace).nospace()
                    << "COMPOSER_UPLOAD_READY itemId=" << itemId
                    << " generation=" << generation
                    << " file=" << upload->path
                    << " fileId=" << fileId;
            }

            guard->updateSendButtonState();
        });
}

bool OutgoingPostCreator::hasAttachmentUploadsInProgress() const
{
    for (auto it = attachmentUploads.cbegin(); it != attachmentUploads.cend(); ++it) {
        if (it->uploading) {
            return true;
        }
    }
    return false;
}

bool OutgoingPostCreator::hasAttachmentUploadFailures() const
{
    for (auto it = attachmentUploads.cbegin(); it != attachmentUploads.cend(); ++it) {
        if (!it->uploading && it->fileId.isEmpty() && !it->error.isEmpty()) {
            return true;
        }
    }
    return false;
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
    } else if (hasAttachmentUploadsInProgress()) {
        sendButtonEnabled = false;
        tooltipText = tr("Uploading attachments…");
	} else if (!isCreatingPost()) {
		sendButtonEnabled = false;
		tooltipText = tr("Cannot send empty message");
    } else if (hasAttachmentUploadFailures()) {
        tooltipText = tr("Send · failed attachment uploads will be retried");
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

void OutgoingPostCreator::setRootId(QString id)
{
	root_id = std::move(id);
}

} /* namespace Mattermost */