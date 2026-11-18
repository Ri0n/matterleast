/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 */

#include "PostWidget.h"

#include <algorithm>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPointer>
#include <QUrl>

#include "MessageContentWidget.h"
#include "PostForwardPolicy.h"
#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendFile.h"
#include "backend/types/BackendUser.h"
#include "chat-area/QuotedPostPreview.h"
#include "navigation/AppNavigationService.h"
#include "ui_PostWidget.h"

namespace Mattermost {

void PostWidget::refreshForwardedInfo()
{
    const ForwardedCopyInfo info = forwardedCopyInfo(post.props);
    if (post.isDeleted || !info.isForwarded) {
        if (forwardedInfoLabel_ && !forwardedInfoLabel_->isHidden()) {
            forwardedInfoLabel_->hide();
            updateGeometry();
            emit dimensionsChanged();
        }
        return;
    }

    if (!forwardedInfoLabel_) {
        forwardedInfoLabel_ = new QLabel(this);
        forwardedInfoLabel_->setObjectName(QStringLiteral("forwardedOriginLabel"));
        forwardedInfoLabel_->setTextFormat(Qt::PlainText);
        forwardedInfoLabel_->setWordWrap(true);
        forwardedInfoLabel_->setForegroundRole(QPalette::WindowText);
        QFont forwardedFont = chatFont_;
        forwardedFont.setItalic(true);
        if (forwardedFont.pointSizeF() > 0) {
            forwardedFont.setPointSizeF(
                std::max(1.0, forwardedFont.pointSizeF() * 0.90));
        }
        forwardedInfoLabel_->setFont(forwardedFont);
        ui->verticalLayout->insertWidget(
            ui->verticalLayout->indexOf(messageContent), forwardedInfoLabel_);
    }

    const BackendUser* originalAuthor = info.originalAuthorId.isEmpty()
        ? nullptr : backend_.getStorage().getUserById(info.originalAuthorId);
    const QString name = originalAuthor
        ? originalAuthor->getDisplayName() : QString();
    const QString text = name.isEmpty()
        ? tr("Forwarded message")
        : tr("Forwarded from %1").arg(name);
    const bool changed = forwardedInfoLabel_->text() != text
        || forwardedInfoLabel_->isHidden();
    forwardedInfoLabel_->setText(text);
    forwardedInfoLabel_->show();
    if (changed) {
        updateGeometry();
        emit dimensionsChanged();
    }

    if (!originalAuthor && !info.originalAuthorId.isEmpty()
        && requestedForwardedAuthorId_ != info.originalAuthorId) {
        requestedForwardedAuthorId_ = info.originalAuthorId;
        QPointer<PostWidget> guard(this);
        UserProfileService::instance(backend_).ensureUser(
            info.originalAuthorId,
            [guard, authorId = info.originalAuthorId](const BackendUser* user) {
            if (guard && user && !guard->post.isDeleted
                && forwardedCopyInfo(guard->post.props).originalAuthorId == authorId) {
                guard->refreshForwardedInfo();
            }
        });
    }
}

void PostWidget::refreshPermalinkPreviews()
{
    for (auto& preview : permalinkPreviews) {
        if (preview) {
            ui->verticalLayout->removeWidget(preview.get());
        }
    }
    permalinkPreviews.clear();

    if (post.isDeleted || !messageContent) {
        return;
    }

    int insertIndex = ui->verticalLayout->indexOf(messageContent) + 1;
    for (const QJsonValue& embedValue : post.embeds) {
        const QJsonObject embed = embedValue.toObject();
        if (embed.value(QStringLiteral("type")).toString()
            != QStringLiteral("permalink")) {
            continue;
        }

        const QJsonObject embedData = embed.value(QStringLiteral("data")).toObject();
        const QJsonObject previewPost = embedData.value(QStringLiteral("post")).toObject();
        if (previewPost.isEmpty()) {
            // Mattermost omits the embedded post when the viewer is not allowed
            // to see it. Do not issue a second client-side fetch that could
            // bypass the server's preview/permission decision.
            continue;
        }

        QString postId = embedData.value(QStringLiteral("post_id")).toString();
        if (postId.isEmpty()) {
            postId = previewPost.value(QStringLiteral("id")).toString();
        }
        if (postId.isEmpty()) {
            continue;
        }

        const QString userId = previewPost.value(QStringLiteral("user_id")).toString();
        QString authorName;
        if (const BackendUser* author = backend_.getStorage().getUserById(userId)) {
            authorName = author->getDisplayName();
        }
        if (authorName.isEmpty()) {
            authorName = previewPost.value(QStringLiteral("props")).toObject()
                .value(QStringLiteral("override_username")).toString();
        }
        if (authorName.isEmpty()) {
            authorName = tr("Message");
        }

        const QString channelName =
            embedData.value(QStringLiteral("channel_display_name")).toString();
        const QString title = channelName.isEmpty()
            ? authorName
            : authorName + QStringLiteral(" · ") + channelName;

        const QJsonObject previewMetadata =
            previewPost.value(QStringLiteral("metadata")).toObject();
        std::list<BackendFile> previewFiles;
        for (const QJsonValue& fileValue :
             previewMetadata.value(QStringLiteral("files")).toArray()) {
            if (fileValue.isObject()) {
                previewFiles.emplace_back(fileValue.toObject());
            }
        }
        const bool hasAttachments =
            !previewPost.value(QStringLiteral("file_ids")).toArray().isEmpty()
            || !previewFiles.empty();

        auto preview = std::make_unique<QuotedPostPreview>(this, 4);
        preview->setPreview(title,
                            previewPost.value(QStringLiteral("message")).toString(),
                            hasAttachments);
        if (!previewFiles.empty()) {
            preview->setInteractiveAttachments(
                backend_, previewFiles, authorName);
        }
        connect(preview.get(),
                &QuotedPostPreview::dimensionsChanged,
                this,
                &PostWidget::dimensionsChanged);
        preview->setActivatedCallback([this, postId] {
            AppNavigationService::instance(backend_).openPost(postId);
        });
        preview->setLinkActivatedCallback([this](const QUrl& url) {
            AppNavigationService::instance(backend_).openUrl(url);
        });

        ui->verticalLayout->insertWidget(insertIndex++, preview.get());
        permalinkPreviews.push_back(std::move(preview));
    }

    if (!permalinkPreviews.empty()) {
        emit dimensionsChanged();
    }
}

} // namespace Mattermost
