/**
 * @file OutgoingPostCreator.h
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

#pragma once

#include <memory>
#include <QBoxLayout>
#include <QHash>
#include <QTemporaryDir>

#include "MessageTextEditWidget.h"
#include "backend/PostResidencyLease.h"
#include "fwd.h"

class QDragEnterEvent;
class QDragMoveEvent;
class QDropEvent;
class QLabel;
class QEvent;
class QMimeData;
class QPushButton;
class QTimer;

namespace Mattermost {

struct BackendNewPollData;
struct OutgoingPostData;

struct AttachmentUploadState {
    QString path;
    QString fileId;
    QString error;
    bool uploading = false;
    quint64 generation = 0;
};
class ChatLogWidget;

class OutgoingPostCreator: public MessageTextEditWidget {
	Q_OBJECT
public:
	explicit OutgoingPostCreator (QWidget *parent = nullptr);
	~OutgoingPostCreator();
public:
	void init(Backend& backend,
	          BackendChannel& channel,
	          ChatLogWidget& chatLogWidget,
	          QBoxLayout* attachmentParent,
	          QLabel& statusLabel,
	          QPushButton& attachButton,
	          QPushButton& addEmojiButton,
	          QPushButton& sendButton);
	void setRootId(QString id);
	const QString& rootId() const { return root_id; }
    void restorePersistentDraft(const QString& draftKey = QString());
    void flushPersistentDraft();
	QString pollCommandTeamId() const;
	void armPollRealtimeAcknowledgement(const BackendNewPollData& pollData);
	void onDragEnterEvent (QDragEnterEvent* event);
	void onDragMoveEvent (QDragMoveEvent* event);
	void onDropEvent (QDropEvent* event);
	void setStatusLabelText (const QString& string);
	const BackendPost* editingPost() const { return postToEdit; }

public slots:
	void onAttachButtonClick ();
	void createPoll ();
	void onPollPostReceived(BackendPost& post);
	void onPostReceived (BackendPost& post);
	void sendPostButtonAction ();
	void postEditInitiated (BackendPost& post);
	void cancelPostEdit ();

signals:
	void postEditFinished ();

protected:
    bool event(QEvent* event) override;
	void insertFromMimeData(const QMimeData* source) override;

private:
	void createAttachmentList(QStringList& files);
    void startAttachmentUpload(const QString& itemId, const QString& path);
    bool hasAttachmentUploadsInProgress() const;
    bool hasAttachmentUploadFailures() const;
	void updateSendButtonState ();
	void setEditingVisual(bool editing);
	void setSendActivityText();
	void finishSend(const QString& confirmedPostId = QString());
	void failSend(const QString& statusText = QString());
    void schedulePersistentDraftSave();
    void savePersistentDraftNow();
    void discardPersistentDraft();
    void failAttachmentUpload(const QString& statusText);
	bool isEditingPost() const;
	bool isCreatingPost ();
	bool isWaitingForPostServerResponse ();

	void startSendPostSequence ();
	void prepareAndSendPost ();
	void sendPost ();

private:
	Backend*							backend = nullptr;
	BackendChannel*						channel = nullptr;
	ChatLogWidget*						chatLogWidget = nullptr;
	QLabel*								statusLabel = nullptr;
	QPushButton*						attachButton = nullptr;
	QPushButton*						addEmojiButton = nullptr;
	QPushButton*						sendButton = nullptr;
	const BackendPost*					postToEdit;
	PostResidencyLease					editResidencyLease;
	OutgoingAttachmentList*				attachmentList;
    QHash<QString, AttachmentUploadState> attachmentUploads;
	std::unique_ptr<OutgoingPostData> 	outgoingPostData;
	std::unique_ptr<QTemporaryDir>		clipboardAttachmentDir;
	bool								sendFailed = false;
	QBoxLayout* 						attachmentParent = nullptr;
	QString						root_id;
    // Non-empty only while editing a local recovered-unsent draft. Ordinary
    // Mattermost drafts remain keyed by (channel, root) as before.
    QString                             activeRecoveredDraftKey;
    QTimer*                             draftSaveTimer = nullptr;
    bool                                suppressDraftPersistence = false;
};

} /* namespace Mattermost */
