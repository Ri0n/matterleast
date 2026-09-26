/**
 * @file UserSearchDialog.cpp
 * @brief Server-backed user search and DM/GM conversation picker.
 */

#include "UserSearchDialog.h"

#include <algorithm>
#include <utility>

#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelIcons.h"
#include "navigation/AppNavigationService.h"
#include "ui_FilterListDialog.h"

namespace Mattermost {

namespace {

constexpr int SearchDelayMs = 250;
constexpr int MinimumSearchLength = 2;
constexpr int ExistingConversationRole = Qt::UserRole + 1;
constexpr int ParticipantIdsRole = Qt::UserRole + 2;
constexpr int MaxRemoteGroupParticipants = 7;

QSet<QString> participantSet(const QStringList& ids)
{
    QSet<QString> result;
    for (const QString& id : ids) {
        if (!id.isEmpty()) {
            result.insert(id);
        }
    }
    return result;
}

} // namespace

UserSearchDialog::UserSearchDialog(Backend& backend,
                                   const FilterListDialogConfig& cfg,
                                   UserSearchOptions options,
                                   const QSet<QString>& disabledUserIds,
                                   QWidget* parent,
                                   Mode mode)
    : UserListDialog(parent)
    , backend(backend)
    , cfg(cfg)
    , searchOptions(std::move(options))
    , disabledUserIds(disabledUserIds)
    , mode(mode)
{
    // This dialog owns filtering itself: local DM/GM rows are matched against
    // the same multi-field predicate as remote users, while remote discovery
    // is debounced and generation-gated. The generic FilterListDialog filter
    // only looks at column 0 (display name).
    setClientSideFilteringEnabled(false);
    setProfileBackend(&backend);
    create(cfg, {}, {QStringLiteral("Full Name"), QStringLiteral("Status")});

    if (mode == Mode::ConversationPicker) {
        setupConversationUi();
        ui->filterLineEdit->setPlaceholderText(
            tr("Filter conversations or search users"));
    } else {
        ui->filterLineEdit->setPlaceholderText(
            QStringLiteral("Filter DMs or search users"));
    }

    searchTimer.setSingleShot(true);
    searchTimer.setInterval(SearchDelayMs);
    connect(&searchTimer, &QTimer::timeout, this, &UserSearchDialog::performSearch);
    connect(ui->filterLineEdit, &QLineEdit::textEdited,
            this, &UserSearchDialog::filterEdited);

    // Local DM/GM conversations are available synchronously. Remote discovery
    // augments them only after the user starts typing.
    showUsers({});
}

UserSearchDialog* UserSearchDialog::showConversationPicker(Backend& backend,
                                                           QWidget* parent)
{
    FilterListDialogConfig config;
    config.title = tr("Start Conversation");
    config.description = tr(
        "Open an existing direct or group conversation, or choose participants "
        "for a new one.");
    config.filterLabelText = tr("Filter or search people");
    // Conversation mode owns its action buttons; FilterListDialog must not
    // replace them on every asynchronous table rebuild.
    config.buttons = QDialogButtonBox::NoButton;
    config.disabledItemTooltip = tr("This user cannot be selected");

    UserSearchOptions options;
    options.limit = 100;

    auto* dialog = new UserSearchDialog(
        backend, config, options, {}, parent, Mode::ConversationPicker);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
    return dialog;
}

void UserSearchDialog::setupConversationUi()
{
    participantHost = new QWidget(this);
    participantHost->setObjectName(QStringLiteral("conversationParticipants"));
    participantLayout = new QHBoxLayout(participantHost);
    participantLayout->setContentsMargins(0, 2, 0, 2);
    participantLayout->setSpacing(5);
    ui->verticalLayout->insertWidget(1, participantHost);

    // The .ui file starts with Ok/Cancel. Conversation mode deliberately uses
    // explicit state-dependent actions instead of a generic "OK".
    ui->buttonBox->setStandardButtons(QDialogButtonBox::Cancel);
    openConversationButton = ui->buttonBox->addButton(
        tr("Open chat"), QDialogButtonBox::ActionRole);
    newConversationButton = ui->buttonBox->addButton(
        tr("New conversation"), QDialogButtonBox::ActionRole);
    openConversationButton->setObjectName(QStringLiteral("openConversationButton"));
    newConversationButton->setObjectName(QStringLiteral("newConversationButton"));

    connect(openConversationButton, &QPushButton::clicked,
            this, &UserSearchDialog::openSelectedConversation);
    connect(newConversationButton, &QPushButton::clicked,
            this, &UserSearchDialog::createConversation);
    connect(ui->tableWidget, &QTableWidget::cellDoubleClicked,
            this, [this](int row, int) { handleConversationDoubleClick(row); });
    connect(ui->tableWidget, &QTableWidget::itemSelectionChanged,
            this, &UserSearchDialog::updateConversationActions);

    rebuildParticipantChips();
    updateConversationActions();
}

const BackendUser* UserSearchDialog::getSelectedUser()
{
    return handledExistingConversation ? nullptr : UserListDialog::getSelectedUser();
}

void UserSearchDialog::setItemCountLabel(uint32_t count)
{
    ui->usersCountLabel->setText(QString::number(count)
        + (count == 1 ? QStringLiteral(" result") : QStringLiteral(" results")));
}

void UserSearchDialog::accept()
{
    if (mode == Mode::ConversationPicker) {
        if (openConversationButton && openConversationButton->isVisible()
            && openConversationButton->isEnabled()) {
            openSelectedConversation();
        } else if (newConversationButton && newConversationButton->isVisible()
                   && newConversationButton->isEnabled()) {
            createConversation();
        }
        return;
    }

    handledExistingConversation = false;

    const auto selection = ui->tableWidget->selectedItems();
    if (!selection.isEmpty()) {
        QTableWidgetItem* firstItem = ui->tableWidget->item(selection.first()->row(), 0);
        if (firstItem) {
            const QString channelId = firstItem->data(ExistingConversationRole).toString();
            if (!channelId.isEmpty()) {
                handledExistingConversation = true;
                AppNavigationService::instance(backend).openChannel(channelId);
                QDialog::accept();
                return;
            }
        }
    }

    const BackendUser* user = UserListDialog::getSelectedUser();
    if (user) {
        if (BackendChannel* channel = backend.getStorage().getDirectChannelByUserId(user->id)) {
            handledExistingConversation = true;
            AppNavigationService::instance(backend).openChannel(channel->id);
        }
    }
    QDialog::accept();
}

bool UserSearchDialog::matchesSearch(const BackendUser& user) const
{
    if (searchTerm.isEmpty()) {
        return true;
    }
    return user.getDisplayName().contains(searchTerm, Qt::CaseInsensitive)
        || user.username.contains(searchTerm, Qt::CaseInsensitive)
        || user.nickname.contains(searchTerm, Qt::CaseInsensitive)
        || user.email.contains(searchTerm, Qt::CaseInsensitive);
}

bool UserSearchDialog::matchesSearch(const BackendChannel& channel) const
{
    return searchTerm.isEmpty()
        || channel.display_name.contains(searchTerm, Qt::CaseInsensitive);
}

void UserSearchDialog::filterEdited(const QString& text)
{
    searchTimer.stop();
    searchTerm = text.trimmed();
    ++searchGeneration;

    // Existing conversations react immediately, even for one character.
    // Participant chips constrain GM results. DMs keep ordinary text-search
    // semantics whenever text is present.
    showUsers({});

    if (searchTerm.size() >= MinimumSearchLength) {
        searchTimer.start();
    }
}

void UserSearchDialog::performSearch()
{
    UserSearchOptions options = searchOptions;
    options.term = searchTerm;
    const int generation = searchGeneration;
    QPointer<UserSearchDialog> guard(this);

    UserProfileService::instance(backend).searchUsers(
        options, [guard, generation](QVector<const BackendUser*> users) mutable {
            if (!guard || generation != guard->searchGeneration) {
                return;
            }

            QStringList userIds;
            userIds.reserve(users.size());
            for (const BackendUser* user : users) {
                if (user) {
                    userIds.push_back(user->id);
                }
            }

            UserProfileService::instance(guard->backend).ensureStatuses(
                userIds, [guard, generation, users = std::move(users)]() mutable {
                    if (!guard || generation != guard->searchGeneration) {
                        return;
                    }
                    guard->showUsers(users);
                });
        });
}

QStringList UserSearchDialog::participantIdsForChannel(
    const BackendChannel& channel) const
{
    QStringList result;
    if (channel.type == BackendChannel::directChannel) {
        if (!channel.name.isEmpty() && channel.name != backend.getLoginUser().id) {
            result.push_back(channel.name);
        }
        return result;
    }

    if (channel.type != BackendChannel::groupChannel) {
        return result;
    }

    const QString selfId = backend.getLoginUser().id;
    for (auto it = channel.members.cbegin(); it != channel.members.cend(); ++it) {
        if (!it.key().isEmpty() && it.key() != selfId) {
            result.push_back(it.key());
        }
    }
    result.sort(Qt::CaseSensitive);
    return result;
}

bool UserSearchDialog::containsSelectedParticipants(
    const QStringList& participantIds) const
{
    const QSet<QString> available = participantSet(participantIds);
    for (const QString& selected : selectedParticipantIds) {
        if (!available.contains(selected)) {
            return false;
        }
    }
    return true;
}

bool UserSearchDialog::exactlyMatchesSelectedParticipants(
    const QStringList& participantIds) const
{
    return !selectedParticipantIds.isEmpty()
        && participantSet(participantIds) == selectedParticipantIds;
}

void UserSearchDialog::ensureGroupParticipants(
    BackendChannel& channel,
    std::function<void()> callback)
{
    if (channel.type != BackendChannel::groupChannel) {
        if (callback) {
            callback();
        }
        return;
    }

    if (hydratedGroupChannels.contains(channel.id) || !channel.members.isEmpty()) {
        hydratedGroupChannels.insert(channel.id);
        if (callback) {
            callback();
        }
        return;
    }

    if (callback) {
        groupMemberWaiters[channel.id].push_back(std::move(callback));
    }
    if (pendingGroupMemberLoads.contains(channel.id)) {
        return;
    }

    pendingGroupMemberLoads.insert(channel.id);
    const QString channelId = channel.id;
    QPointer<UserSearchDialog> guard(this);
    UserProfileService::instance(backend).ensureGroupChannelMembers(
        channel, [guard, channelId] {
            if (!guard) {
                return;
            }

            guard->pendingGroupMemberLoads.remove(channelId);
            guard->hydratedGroupChannels.insert(channelId);
            const auto waiters = guard->groupMemberWaiters.take(channelId);
            for (const auto& waiter : waiters) {
                if (waiter) {
                    waiter();
                }
            }

            // Participant filtering of GMs needs the authoritative small
            // member set regardless of whether text search is also active.
            if (guard->mode == Mode::ConversationPicker
                && !guard->selectedParticipantIds.isEmpty()) {
                guard->showUsers(guard->lastServerUsers);
            }
        });
}

void UserSearchDialog::showUsers(const QVector<const BackendUser*>& serverUsers)
{
    lastServerUsers = serverUsers;

    const QString selectedConversation =
        !preferredConversationId.isEmpty()
            ? preferredConversationId
            : conversationIdForRow(ui->tableWidget->currentRow());

    const bool hasSelectedParticipants = mode == Mode::ConversationPicker
        && !selectedParticipantIds.isEmpty();
    const bool participantOnlyFilter = hasSelectedParticipants
        && searchTerm.isEmpty();

    std::set<UserListEntry> entries;
    QSet<QString> addedUserIds;
    const QString currentUserId = backend.getLoginUser().id;

    const auto& directChannels = backend.getStorage().directChannelsByUser;
    for (auto it = directChannels.constBegin(); it != directChannels.constEnd(); ++it) {
        const QString userId = it.key();
        const BackendUser* user = backend.getStorage().getUserById(userId);
        if (!user || user->id == currentUserId) {
            continue;
        }

        if (participantOnlyFilter) {
            if (selectedParticipantIds.size() != 1
                || !selectedParticipantIds.contains(user->id)) {
                continue;
            }
        } else if (!matchesSearch(*user)) {
            continue;
        }

        entries.emplace(user, false);
        addedUserIds.insert(user->id);
    }

    // Remote user rows are discovery candidates, not existing conversations.
    // Participant-only mode intentionally suppresses them and shows only DMs/GMs
    // containing the chosen participants.
    if (!participantOnlyFilter) {
        for (const BackendUser* user : serverUsers) {
            if (!user || user->id == currentUserId
                || addedUserIds.contains(user->id)
                || !matchesSearch(*user)) {
                continue;
            }
            entries.emplace(user, disabledUserIds.contains(user->id));
            addedUserIds.insert(user->id);
        }
    }

    dataToItemMap.clear();
    ui->tableWidget->clearContents();
    create(cfg, entries, {QStringLiteral("Full Name"), QStringLiteral("Status")});

    // Give every person row an explicit participant identity. Existing DMs also
    // carry their channel id so the dynamic action area can distinguish "Open"
    // from "New conversation".
    for (int row = 0; row < ui->tableWidget->rowCount(); ++row) {
        QTableWidgetItem* item = ui->tableWidget->item(row, 0);
        const BackendUser* user = item
            ? item->data(Qt::UserRole).value<BackendUser*>() : nullptr;
        if (!item || !user) {
            continue;
        }

        item->setData(ParticipantIdsRole, QStringList {user->id});
        const auto directIt =
            backend.getStorage().directChannelsByUser.constFind(user->id);
        if (directIt != backend.getStorage().directChannelsByUser.cend()
            && directIt.value()) {
            item->setData(ExistingConversationRole, directIt.value()->id);
        }
    }

    QVector<BackendChannel*> groupChannels;
    groupChannels.reserve(
        static_cast<int>(backend.getStorage().groupChannels.channels.size()));
    for (const auto& ownedChannel : backend.getStorage().groupChannels.channels) {
        BackendChannel* channel = ownedChannel.get();
        if (!channel) {
            continue;
        }

        if (hasSelectedParticipants) {
            if (!hydratedGroupChannels.contains(channel->id)
                && channel->members.isEmpty()) {
                ensureGroupParticipants(*channel);
                continue;
            }
            if (!containsSelectedParticipants(
                    participantIdsForChannel(*channel))) {
                continue;
            }
        }
        if (!matchesSearch(*channel)) {
            continue;
        }

        groupChannels.push_back(channel);
    }

    std::sort(groupChannels.begin(), groupChannels.end(),
              [](const BackendChannel* lhs, const BackendChannel* rhs) {
        return lhs->display_name.compare(
            rhs->display_name, Qt::CaseInsensitive) < 0;
    });

    for (BackendChannel* channel : std::as_const(groupChannels)) {
        const int row = ui->tableWidget->rowCount();
        ui->tableWidget->insertRow(row);

        auto* nameItem = new QTableWidgetItem(
            ChannelIcons::groupConversation(), channel->display_name);
        nameItem->setData(ExistingConversationRole, channel->id);
        nameItem->setData(ParticipantIdsRole,
                          participantIdsForChannel(*channel));
        ui->tableWidget->setItem(row, 0, nameItem);
        ui->tableWidget->setItem(
            row, 1, new QTableWidgetItem(tr("Group conversation")));
    }

    setItemCountLabel(static_cast<uint32_t>(ui->tableWidget->rowCount()));

    if (!selectedConversation.isEmpty()) {
        for (int row = 0; row < ui->tableWidget->rowCount(); ++row) {
            if (conversationIdForRow(row) == selectedConversation) {
                ui->tableWidget->selectRow(row);
                break;
            }
        }
    }
    preferredConversationId.clear();
    updateConversationActions();
}

QString UserSearchDialog::conversationIdForRow(int row) const
{
    if (row < 0 || row >= ui->tableWidget->rowCount()) {
        return {};
    }
    QTableWidgetItem* item = ui->tableWidget->item(row, 0);
    return item ? item->data(ExistingConversationRole).toString() : QString();
}

QStringList UserSearchDialog::participantIdsForRow(int row) const
{
    if (row < 0 || row >= ui->tableWidget->rowCount()) {
        return {};
    }
    QTableWidgetItem* item = ui->tableWidget->item(row, 0);
    return item ? item->data(ParticipantIdsRole).toStringList() : QStringList();
}

void UserSearchDialog::handleConversationDoubleClick(int row)
{
    if (mode != Mode::ConversationPicker || row < 0) {
        return;
    }

    QTableWidgetItem* item = ui->tableWidget->item(row, 0);
    if (!item) {
        return;
    }

    const QString conversationId = conversationIdForRow(row);
    if (BackendUser* user = item->data(Qt::UserRole).value<BackendUser*>()) {
        addParticipants(QStringList {user->id}, conversationId);
        return;
    }

    BackendChannel* channel = conversationId.isEmpty()
        ? nullptr : backend.getStorage().getChannelById(conversationId);
    if (!channel || channel->type != BackendChannel::groupChannel) {
        return;
    }

    QPointer<UserSearchDialog> guard(this);
    ensureGroupParticipants(*channel, [guard, conversationId] {
        if (!guard) {
            return;
        }
        BackendChannel* current =
            guard->backend.getStorage().getChannelById(conversationId);
        if (current) {
            guard->addParticipants(
                guard->participantIdsForChannel(*current), conversationId);
        }
    });
}

void UserSearchDialog::addParticipants(
    const QStringList& userIds,
    const QString& preferredId)
{
    const QString selfId = backend.getLoginUser().id;
    bool changed = false;
    for (const QString& userId : userIds) {
        if (userId.isEmpty() || userId == selfId
            || selectedParticipantIds.contains(userId)) {
            continue;
        }
        selectedParticipantIds.insert(userId);
        changed = true;
    }

    if (!preferredId.isEmpty()) {
        preferredConversationId = preferredId;
    }
    if (!changed && preferredId.isEmpty()) {
        return;
    }

    rebuildParticipantChips();
    showUsers(lastServerUsers);
}

void UserSearchDialog::removeParticipant(const QString& userId)
{
    if (!selectedParticipantIds.remove(userId)) {
        return;
    }
    preferredConversationId.clear();
    rebuildParticipantChips();
    showUsers(lastServerUsers);
}

void UserSearchDialog::rebuildParticipantChips()
{
    if (!participantLayout) {
        return;
    }

    while (QLayoutItem* item = participantLayout->takeAt(0)) {
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    auto* title = new QLabel(tr("Participants:"), participantHost);
    title->setObjectName(QStringLiteral("participantsLabel"));
    participantLayout->addWidget(title);

    struct NamedId {
        QString name;
        QString id;
    };
    QVector<NamedId> participants;
    participants.reserve(selectedParticipantIds.size());
    for (const QString& id : selectedParticipantIds) {
        const BackendUser* user = backend.getStorage().getUserById(id);
        participants.push_back(
            {user ? user->getDisplayName() : id, id});
    }
    std::sort(participants.begin(), participants.end(),
              [](const NamedId& lhs, const NamedId& rhs) {
        return lhs.name.compare(rhs.name, Qt::CaseInsensitive) < 0;
    });

    for (const NamedId& participant : std::as_const(participants)) {
        auto* chip = new QFrame(participantHost);
        chip->setFrameShape(QFrame::StyledPanel);
        chip->setObjectName(QStringLiteral("participantChip"));
        auto* chipLayout = new QHBoxLayout(chip);
        chipLayout->setContentsMargins(6, 1, 2, 1);
        chipLayout->setSpacing(2);

        auto* label = new QLabel(participant.name, chip);
        chipLayout->addWidget(label);

        auto* remove = new QToolButton(chip);
        remove->setText(QStringLiteral("×"));
        remove->setAutoRaise(true);
        remove->setFixedSize(18, 18);
        remove->setToolTip(tr("Remove %1").arg(participant.name));
        remove->setAccessibleName(remove->toolTip());
        connect(remove, &QToolButton::clicked, this,
                [this, id = participant.id] { removeParticipant(id); });
        chipLayout->addWidget(remove);

        participantLayout->addWidget(chip);
    }
    participantLayout->addStretch(1);
}

void UserSearchDialog::updateConversationActions()
{
    if (mode != Mode::ConversationPicker
        || !openConversationButton || !newConversationButton) {
        return;
    }

    const int row = ui->tableWidget->currentRow();
    const QString conversationId = conversationIdForRow(row);
    const QStringList rowParticipants = participantIdsForRow(row);
    const bool hasChosenParticipants = !selectedParticipantIds.isEmpty();
    const bool exactExisting = !conversationId.isEmpty()
        && exactlyMatchesSelectedParticipants(rowParticipants);

    bool showOpen = false;
    bool showNew = false;

    if (hasChosenParticipants) {
        // Exact participant identity is the only case where an existing GM/DM
        // is semantically the requested conversation. A superset match is only
        // a discovery aid and must not turn into "Open".
        showOpen = exactExisting;
        showNew = !exactExisting;
    } else if (row >= 0) {
        showOpen = !conversationId.isEmpty();
        QTableWidgetItem* item = ui->tableWidget->item(row, 0);
        const BackendUser* user = item
            ? item->data(Qt::UserRole).value<BackendUser*>() : nullptr;
        showNew = !showOpen && user;
    }

    openConversationButton->setVisible(showOpen);
    openConversationButton->setEnabled(showOpen);
    newConversationButton->setVisible(showNew);

    QStringList targetIds;
    if (hasChosenParticipants) {
        targetIds = selectedParticipantIds.values();
    } else if (row >= 0) {
        QTableWidgetItem* item = ui->tableWidget->item(row, 0);
        if (const BackendUser* user =
                item ? item->data(Qt::UserRole).value<BackendUser*>() : nullptr) {
            targetIds.push_back(user->id);
        }
    }

    const bool validCount = !targetIds.isEmpty()
        && targetIds.size() <= MaxRemoteGroupParticipants;
    newConversationButton->setEnabled(showNew && validCount);
    newConversationButton->setToolTip(
        targetIds.size() > MaxRemoteGroupParticipants
            ? tr("A group conversation can contain at most 8 people including you.")
            : QString());
}

void UserSearchDialog::openSelectedConversation()
{
    const QString channelId =
        conversationIdForRow(ui->tableWidget->currentRow());
    if (channelId.isEmpty()) {
        return;
    }

    AppNavigationService::instance(backend).openChannel(channelId);
    QDialog::accept();
}

void UserSearchDialog::createConversation()
{
    QStringList userIds;
    if (!selectedParticipantIds.isEmpty()) {
        userIds = selectedParticipantIds.values();
    } else {
        QTableWidgetItem* item =
            ui->tableWidget->item(ui->tableWidget->currentRow(), 0);
        const BackendUser* user = item
            ? item->data(Qt::UserRole).value<BackendUser*>() : nullptr;
        if (user) {
            userIds.push_back(user->id);
        }
    }

    userIds.removeAll(backend.getLoginUser().id);
    userIds.removeDuplicates();
    if (userIds.isEmpty()
        || userIds.size() > MaxRemoteGroupParticipants) {
        return;
    }

    QPointer<UserSearchDialog> guard(this);
    if (userIds.size() == 1) {
        const QString userId = userIds.first();
        if (BackendChannel* existing =
                backend.getStorage().getDirectChannelByUserId(userId)) {
            AppNavigationService::instance(backend).openChannel(existing->id);
            QDialog::accept();
            return;
        }

        BackendUser* user = backend.getStorage().getUserById(userId);
        if (!user) {
            return;
        }
        backend.createDirectChannel(
            *user, [guard](BackendChannel& channel) {
                if (!guard) {
                    return;
                }
                AppNavigationService::instance(guard->backend).openChannel(channel.id);
                guard->QDialog::accept();
            });
        return;
    }

    backend.createGroupChannel(
        userIds, [guard](BackendChannel& channel) {
            if (!guard) {
                return;
            }
            AppNavigationService::instance(guard->backend).openChannel(channel.id);
            guard->QDialog::accept();
        });
}

} // namespace Mattermost
