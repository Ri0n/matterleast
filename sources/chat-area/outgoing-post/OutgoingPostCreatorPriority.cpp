/**
 * Copyright 2026 Sergei Ilinykh
 *
 * Message-priority composer controls and wire metadata staging.
 */

#include "OutgoingPostCreator.h"

#include <QAction>
#include <QActionGroup>
#include <QCursor>
#include <QJsonObject>
#include <QMenu>
#include <QPushButton>

#include "backend/PendingPostService.h"
#include "backend/PostCreateService.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

bool OutgoingPostCreator::messagePriorityAvailable()
{
    // Mattermost message priority is defined for newly created root posts.
    return backend && channel && root_id.isEmpty()
        && !isEditingPost() && !isWaitingForPostServerResponse();
}

QJsonObject OutgoingPostCreator::currentPostMetadata() const
{
    if (messagePriority.isEmpty()) {
        return {};
    }

    QJsonObject priority {
        {QStringLiteral("priority"), messagePriority},
        {QStringLiteral("requested_ack"), priorityRequestedAck},
    };
    if (messagePriority == QStringLiteral("urgent")) {
        priority.insert(
            QStringLiteral("persistent_notifications"),
            priorityPersistentNotifications);
    }

    return QJsonObject {
        {QStringLiteral("priority"), priority},
    };
}

void OutgoingPostCreator::updateMessagePriorityButtonState()
{
    QWidget* host = parentWidget();
    auto* button = host
        ? host->findChild<QPushButton*>(QStringLiteral("messagePriorityButton"))
        : nullptr;
    if (!button) {
        return;
    }

    button->setCheckable(true);
    button->setChecked(!messagePriority.isEmpty());
    if (messagePriority == QStringLiteral("important")) {
        button->setToolTip(tr("Message priority: Important"));
    } else if (messagePriority == QStringLiteral("urgent")) {
        button->setToolTip(tr("Message priority: Urgent"));
    } else {
        button->setToolTip(tr("Message priority"));
    }
}

void OutgoingPostCreator::resetMessagePriority()
{
    messagePriority.clear();
    priorityRequestedAck = false;
    priorityPersistentNotifications = false;
    updateMessagePriorityButtonState();
}

void OutgoingPostCreator::ensurePriorityOutboxHook()
{
    if (priorityOutboxHookInstalled || !backend) {
        return;
    }

    auto& outbox = PendingPostService::instance(*backend);
    auto* postCreate = &PostCreateService::instance(*backend);
    connect(&outbox, &PendingPostService::postAdded,
            this,
            [this, postCreate](const QString& channelId,
                               const QString& rootId,
                               const QString& pendingPostId) {
        if (messagePriority.isEmpty()
            || !channel
            || channelId != channel->id
            || rootId != root_id
            || !isWaitingForPostServerResponse()) {
            return;
        }

        postCreate->stagePendingPostMetadata(
            pendingPostId, currentPostMetadata());
        resetMessagePriority();
    });
    priorityOutboxHookInstalled = true;
}

void OutgoingPostCreator::showMessagePriorityMenu()
{
    QMenu menu(this);

    if (!messagePriorityAvailable()) {
        QAction* unavailable = menu.addAction(
            tr("Message priority is available for new root messages"));
        unavailable->setEnabled(false);
        menu.exec(QCursor::pos());
        return;
    }

    ensurePriorityOutboxHook();

    QActionGroup priorityGroup(&menu);
    priorityGroup.setExclusive(true);

    QAction* standard = menu.addAction(tr("Standard"));
    QAction* important = menu.addAction(tr("Important"));
    QAction* urgent = menu.addAction(tr("Urgent"));
    for (QAction* action : {standard, important, urgent}) {
        action->setCheckable(true);
        priorityGroup.addAction(action);
    }

    standard->setChecked(messagePriority.isEmpty());
    important->setChecked(messagePriority == QStringLiteral("important"));
    urgent->setChecked(messagePriority == QStringLiteral("urgent"));

    menu.addSeparator();
    QAction* requestedAck = menu.addAction(tr("Request acknowledgement"));
    requestedAck->setCheckable(true);
    QAction* persistentNotifications =
        menu.addAction(tr("Persistent notifications"));
    persistentNotifications->setCheckable(true);

    const auto syncOptions = [this, requestedAck, persistentNotifications] {
        const bool prioritized = !messagePriority.isEmpty();
        requestedAck->setEnabled(prioritized);
        requestedAck->setChecked(prioritized && priorityRequestedAck);
        persistentNotifications->setEnabled(
            messagePriority == QStringLiteral("urgent"));
        persistentNotifications->setChecked(
            messagePriority == QStringLiteral("urgent")
            && priorityPersistentNotifications);
        updateMessagePriorityButtonState();
    };

    connect(standard, &QAction::triggered, this, [this, syncOptions] {
        messagePriority.clear();
        priorityRequestedAck = false;
        priorityPersistentNotifications = false;
        syncOptions();
    });
    connect(important, &QAction::triggered, this, [this, syncOptions] {
        messagePriority = QStringLiteral("important");
        priorityPersistentNotifications = false;
        syncOptions();
    });
    connect(urgent, &QAction::triggered, this, [this, syncOptions] {
        const bool newlyUrgent = messagePriority != QStringLiteral("urgent");
        messagePriority = QStringLiteral("urgent");
        if (newlyUrgent) {
            priorityRequestedAck = true;
        }
        syncOptions();
    });
    connect(requestedAck, &QAction::toggled,
            this, [this](bool enabled) {
        if (!messagePriority.isEmpty()) {
            priorityRequestedAck = enabled;
            updateMessagePriorityButtonState();
        }
    });
    connect(persistentNotifications, &QAction::toggled,
            this, [this](bool enabled) {
        if (messagePriority == QStringLiteral("urgent")) {
            priorityPersistentNotifications = enabled;
            updateMessagePriorityButtonState();
        }
    });

    syncOptions();
    menu.exec(QCursor::pos());
}

} // namespace Mattermost
