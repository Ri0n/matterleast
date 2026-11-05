/**
 * @file ViewChannelMembersListDialog.cpp
 * @brief 'View Channel Members' context menu item dialog
 * @author Lyubomir Filipov
 * @date Apr 17, 2023
 *
 * Copyright 2021, 2022 Lyubomir Filipov
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

#include "ViewChannelMembersListDialog.h"

#include "ChannelMemberPagePolicy.h"
#include "UserSearchDialog.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <utility>

#include <QEvent>
#include <QDateTime>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSizePolicy>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendChannelMember.h"
#include "backend/types/BackendUser.h"
#include "ui/AvatarUtils.h"
#include "ui/EmojiPresentation.h"
#include "ui_FilterListDialog.h"
#include "widgets/LongListWidget.h"

namespace Mattermost {
namespace {

constexpr int ChannelMemberPageSize = 50;
constexpr int MemberRowHeight = 38;
constexpr int MemberAvatarSize = 28;
constexpr int MemberPresenceBadgeSize = 7;
constexpr int MemberSearchDelayMs = 200;
constexpr int MemberSearchLimit = 100;

QString memberDisplayName(const BackendUser* user, const QString& fallback)
{
    if (!user) {
        return fallback;
    }
    QString name = user->getDisplayName();
    if (name.isEmpty()) {
        name = user->username;
    }
    if (!user->nickname.isEmpty()) {
        name += QStringLiteral(" (") + user->nickname + QLatin1Char(')');
    }
    return name;
}

QStringList splitRoleNames(const QString& roles)
{
    // Mattermost role names are whitespace separated; legacy persisted
    // profile data may also contain commas.
    return roles.split(QRegularExpression(QStringLiteral("[\\s,]+")),
                       Qt::SkipEmptyParts);
}

QString channelMemberRole(const BackendChannelMember* member,
                          const BackendUser* user)
{
    // Never guess a channel role from an unrelated team/user role if the
    // channel membership record has not been loaded yet.
    if (!member) {
        return QObject::tr("Unknown");
    }

    if (user && splitRoleNames(user->roles.join(QLatin1Char(' ')))
                    .contains(QStringLiteral("system_admin"))) {
        return QObject::tr("System admin");
    }
    const QStringList roles = splitRoleNames(member->roles);
    if (member->isAdmin || roles.contains(QStringLiteral("channel_admin"))) {
        return QObject::tr("Channel admin");
    }
    if (roles.contains(QStringLiteral("channel_guest"))
        || (user && splitRoleNames(user->roles.join(QLatin1Char(' ')))
                        .contains(QStringLiteral("system_guest")))) {
        return QObject::tr("Guest");
    }
    return QObject::tr("Member");
}

bool matchesUser(const BackendUser& user, const QString& term)
{
    return term.isEmpty()
        || user.getDisplayName().contains(term, Qt::CaseInsensitive)
        || user.username.contains(term, Qt::CaseInsensitive)
        || user.nickname.contains(term, Qt::CaseInsensitive)
        || user.email.contains(term, Qt::CaseInsensitive);
}

class ChannelMemberRow final : public QWidget
{
public:
    ChannelMemberRow(Backend& backend,
                     const BackendUser* user,
                     QString fallbackName,
                     QString role,
                     std::function<void(const BackendUser*, const QPoint&)> contextMenu,
                     QWidget* parent = nullptr)
        : QWidget(parent)
        , backend_(backend)
        , user_(user)
        , fallbackName_(std::move(fallbackName))
        , role_(std::move(role))
        , contextMenu_(std::move(contextMenu))
    {
        setMinimumHeight(MemberRowHeight);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setObjectName(QStringLiteral("channelMemberRow"));
        setProperty("memberUserId", user_ ? user_->id : fallbackName_);
        setContextMenuPolicy(Qt::CustomContextMenu);

        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 3, 8, 3);
        layout->setSpacing(8);

        avatar_ = new QLabel(this);
        avatar_->setFixedSize(MemberAvatarSize, MemberAvatarSize);
        avatar_->setAlignment(Qt::AlignCenter);
        layout->addWidget(avatar_);

        name_ = new QLabel(this);
        name_->setTextInteractionFlags(Qt::NoTextInteraction);
        layout->addWidget(name_, 3);

        roleLabel_ = new QLabel(this);
        roleLabel_->setObjectName(QStringLiteral("memberRole"));
        roleLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
        layout->addWidget(roleLabel_, 1);

        customStatus_ = new QLabel(this);
        customStatus_->setObjectName(QStringLiteral("memberCustomStatus"));
        customStatus_->setTextFormat(Qt::RichText);
        customStatus_->setMinimumWidth(0);
        customStatus_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        layout->addWidget(customStatus_, 2);

        connect(this, &QWidget::customContextMenuRequested, this,
                [this](const QPoint& pos) {
            if (contextMenu_) {
                contextMenu_(user_, mapToGlobal(pos));
            }
        });

        if (user_) {
            connect(user_, &BackendUser::onAvatarChanged,
                    this, [this] { updateAvatar(); });
            connect(user_, &BackendUser::onStatusChanged,
                    this, [this] { updateText(); updateAvatar(); });
            connect(user_, &BackendUser::onCustomStatusChanged,
                    this, [this] { updateText(); });
            connect(&backend_.emojiRegistry(), &EmojiRegistry::customEmojiAdded,
                    this, [this](const QString&) { updateText(); });
            UserProfileService::instance(backend_).ensureAvatar(*user_);
        }

        updateText();
        updateAvatar();
    }

    void setRole(QString role)
    {
        role_ = std::move(role);
        roleLabel_->setText(role_);
    }

protected:
    void changeEvent(QEvent* event) override
    {
        QWidget::changeEvent(event);
        if (event && (event->type() == QEvent::PaletteChange
                      || event->type() == QEvent::ApplicationPaletteChange
                      || event->type() == QEvent::StyleChange)) {
            updateAvatar();
        }
    }

private:
    void updateText()
    {
        name_->setText(memberDisplayName(user_, fallbackName_));
        roleLabel_->setText(role_);
        customStatus_->clear();
        customStatus_->setToolTip({});
        if (!user_) return;
        const auto& custom = user_->props.customStatus;
        const auto expiry = QDateTime::fromString(custom.expiresAt, Qt::ISODateWithMs);
        if (expiry.isValid() && expiry <= QDateTime::currentDateTimeUtc()) return;
        QString icon;
        if (!custom.emoji.isEmpty()) {
            const auto emoji = backend_.emojiRegistry().resolveByName(custom.emoji);
            icon = emoji ? emoji->unicodeString : QStringLiteral(":%1:").arg(custom.emoji);
            icon = icon.contains(QStringLiteral("<img"), Qt::CaseInsensitive)
                ? EmojiPresentation::normalizeHtml(icon, font(), EmojiPresentation::Mode::Reaction,
                                                   &backend_.emojiRegistry())
                : icon.toHtmlEscaped();
        }
        customStatus_->setText(icon + (icon.isEmpty() || custom.text.isEmpty() ? QString() : QStringLiteral(" "))
                               + custom.text.toHtmlEscaped());
        customStatus_->setToolTip(custom.text);
    }

    void updateAvatar()
    {
        if (!user_ || user_->avatar.isNull()) {
            avatar_->clear();
            return;
        }
        avatar_->setPixmap(AvatarUtils::withStatus(
            user_->avatar,
            MemberAvatarSize,
            user_->status,
            MemberPresenceBadgeSize,
            palette().color(QPalette::Window)));
    }

    Backend& backend_;
    const BackendUser* user_ = nullptr;
    QString fallbackName_;
    QString role_;
    std::function<void(const BackendUser*, const QPoint&)> contextMenu_;
    QLabel* avatar_ = nullptr;
    QLabel* name_ = nullptr;
    QLabel* roleLabel_ = nullptr;
    QLabel* customStatus_ = nullptr;
};

class ChannelMemberLongList final : public LongListWidget
{
public:
    using Factory = std::function<QWidget*(int)>;

    ChannelMemberLongList(Factory factory, QWidget* parent)
        : LongListWidget(parent)
        , factory_(std::move(factory))
    {
    }

protected:
    QWidget* createItemWidget(int index) override
    {
        return factory_ ? factory_(index) : nullptr;
    }

private:
    Factory factory_;
};

} // namespace

ViewChannelMembersListDialog::ViewChannelMembersListDialog(
    Backend& backend,
    BackendChannel& channel,
    QWidget* parent)
    : UserListDialog(parent)
    , channel(channel)
    , backend(backend)
{
    setProfileBackend(&backend);
    setupVirtualList();
    resetPagedMembers();
    refreshAddPermission();
    refreshRolePermission();
    connect(&channel, &BackendChannel::onUpdated, this, [this] {
        refreshAddPermission();
        refreshRolePermission();
    });

    connect(&channel, &BackendChannel::onUserRemoved, this,
            [this](const BackendUser&) {
                resetPagedMembers();
                refreshAddPermission();
                refreshRolePermission();
            });
    connect(&channel, &BackendChannel::onUserAdded, this,
            [this](const BackendUser&) { resetPagedMembers(); });
}

ViewChannelMembersListDialog::~ViewChannelMembersListDialog()
{
    // Child widgets are deleted by QWidget after our members have been torn
    // down. Their destroyed callbacks must not touch this derived object then.
    if (memberPicker) disconnect(memberPicker, nullptr, this, nullptr);
    finishAllRequests();
}

void ViewChannelMembersListDialog::setupVirtualList()
{
    FilterListDialogConfig dialogCfg {
        QStringLiteral("Channel Members - Mattermost"),
        QStringLiteral("Members of channel '") + channel.display_name + QStringLiteral("':"),
        QStringLiteral("Filter users by name:"),
        {QDialogButtonBox::Close},
        QString(),
    };
    FilterListDialog::create(dialogCfg);
    addMembersButton = ui->buttonBox->addButton(tr("Add members…"), QDialogButtonBox::ActionRole);
    addMembersButton->setObjectName(QStringLiteral("addChannelMembersButton"));
    addMembersButton->hide();
    connect(addMembersButton, &QPushButton::clicked,
            this, &ViewChannelMembersListDialog::addMembers);

    ui->tableWidget->hide();
    const int tableIndex = ui->verticalLayout->indexOf(ui->tableWidget);

    QWidget* header = createHeaderRow();
    ui->verticalLayout->insertWidget(std::max(0, tableIndex), header);

    memberList = new ChannelMemberLongList(
        [this](int index) { return createMemberRow(index); }, this);
    memberList->setDefaultItemHeight(MemberRowHeight);
    memberList->setMaterializationLimit(120);
    memberList->setRequestBlockSize(24);
    memberList->setPrefetchScreens(1);
    ui->verticalLayout->insertWidget(std::max(0, tableIndex) + 1, memberList, 1);

    connect(memberList, &LongListWidget::rangeRequested, this,
            [this](int first,
                   int last,
                   LongListWidget::RequestReason,
                   quint64) {
        requestMemberRange(first, last);
    });

    searchTimer = new QTimer(this);
    searchTimer->setSingleShot(true);
    searchTimer->setInterval(MemberSearchDelayMs);
    connect(searchTimer, &QTimer::timeout,
            this, &ViewChannelMembersListDialog::performSearch);
    connect(ui->filterLineEdit, &QLineEdit::textEdited,
            this, &ViewChannelMembersListDialog::filterEdited);
}

void ViewChannelMembersListDialog::refreshAddPermission()
{
    const int generation = ++permissionGeneration;
    addMembersButton->hide();
    QPointer<ViewChannelMembersListDialog> guard(this);
    backend.canAddChannelMembers(channel, [guard, generation](bool allowed) {
        if (!guard || generation != guard->permissionGeneration) return;
        guard->addMembersButton->setVisible(allowed);
        guard->addMembersButton->setEnabled(allowed && !guard->memberPicker);
    });
}

void ViewChannelMembersListDialog::refreshRolePermission()
{
    const int generation = ++rolePermissionGeneration;
    canEditRoles = false;
    QPointer<ViewChannelMembersListDialog> guard(this);
    backend.canManageChannelRoles(channel, [guard, generation](bool allowed) {
        if (!guard || generation != guard->rolePermissionGeneration) {
            return;
        }
        guard->canEditRoles = allowed;
    });
}

void ViewChannelMembersListDialog::setMemberRole(const QString& userId, bool admin)
{
    if (!canEditRoles || pendingRoleUpdates.contains(userId)
        || !channel.members.contains(userId)) {
        return;
    }
    pendingRoleUpdates.insert(userId);
    QPointer<ViewChannelMembersListDialog> guard(this);
    backend.setChannelMemberAdmin(channel, userId, admin,
        [guard, userId](bool success, const QString& error) {
        if (!guard) {
            return;
        }
        guard->pendingRoleUpdates.remove(userId);
        if (!success) {
            QMessageBox::warning(guard, guard->tr("Change channel role"),
                error.isEmpty() ? guard->tr("Could not change the member's role.")
                                : error);
            return;
        }

        const auto it = guard->channel.members.constFind(userId);
        if (it == guard->channel.members.cend()) {
            return;
        }
        const BackendUser* user = guard->backend.getStorage().getUserById(userId);
        const QString role = channelMemberRole(&it.value(), user);
        // Only rows currently materialized by LongList need updating. Newly
        // materialized rows will read the canonical channel membership.
        for (QWidget* row : guard->findChildren<QWidget*>(
                 QStringLiteral("channelMemberRow"))) {
            if (row->property("memberUserId").toString() == userId) {
                static_cast<ChannelMemberRow*>(row)->setRole(role);
            }
        }
    });
}

void ViewChannelMembersListDialog::addMembers()
{
    if (memberPicker) {
        memberPicker->raise();
        memberPicker->activateWindow();
        return;
    }
    memberPicker = UserSearchDialog::showChannelMemberPicker(backend, channel, this);
    addMembersButton->setEnabled(false);
    connect(memberPicker, &QObject::destroyed, this, [this] {
        memberPicker.clear();
        refreshAddPermission();
    });
}

QWidget* ViewChannelMembersListDialog::createHeaderRow()
{
    auto* header = new QWidget(this);
    auto* layout = new QHBoxLayout(header);
    layout->setContentsMargins(6, 0, 8, 0);
    layout->setSpacing(8);

    auto* avatarSpacer = new QWidget(header);
    avatarSpacer->setFixedWidth(MemberAvatarSize);
    layout->addWidget(avatarSpacer);

    auto addHeader = [layout, header](
        const QString& text, int stretch, const QString& objectName = QString()) {
        auto* label = new QLabel(text, header);
        if (!objectName.isEmpty()) {
            label->setObjectName(objectName);
        }
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        layout->addWidget(label, stretch);
    };
    addHeader(tr("Full Name"), 3);
    addHeader(tr("Role"), 1, QStringLiteral("memberRoleHeader"));
    addHeader(tr("Custom status"), 2);
    return header;
}

void ViewChannelMembersListDialog::reloadMemberCount()
{
    const int generation = memberGeneration;
    QPointer<ViewChannelMembersListDialog> guard(this);
    UserProfileService::instance(backend).queryChannelMemberCount(
        channel, [guard, generation](int count) {
            if (!guard || generation != guard->memberGeneration) {
                return;
            }

            guard->memberCount = std::max(0, count);
            guard->memberIds.resize(guard->memberCount);
            if (!guard->searchMode) {
                guard->memberList->setItemCount(guard->memberCount);
                guard->reapplyLoadedPages();
                guard->setItemCountLabel(
                    static_cast<uint32_t>(guard->memberCount));
            }
        });
}

void ViewChannelMembersListDialog::resetPagedMembers()
{
    ++memberGeneration;
    finishAllRequests();
    loadedPages.clear();
    inFlightPages.clear();
    memberIds.clear();
    memberCount = 0;

    if (!searchMode && memberList) {
        memberList->setItemCount(0);
        setItemCountLabel(0);
    }
    reloadMemberCount();
}

void ViewChannelMembersListDialog::requestMemberRange(int first, int last)
{
    if (!memberList || searchMode || first < 0 || last < first) {
        if (memberList && first >= 0 && last >= first) {
            memberList->finishRangeRequest(first, last);
        }
        return;
    }

    pendingRanges.push_back(PendingRange {first, last});
    const int firstPage = first / ChannelMemberPageSize;
    const int lastPage = last / ChannelMemberPageSize;
    for (int page = firstPage; page <= lastPage; ++page) {
        loadMemberPage(page);
    }
    finishSatisfiedRequests();
}

void ViewChannelMembersListDialog::loadMemberPage(int page)
{
    if (page < 0 || loadedPages.contains(page) || inFlightPages.contains(page)) {
        return;
    }

    inFlightPages.insert(page);
    const int generation = memberGeneration;
    QPointer<ViewChannelMembersListDialog> guard(this);
    UserProfileService::instance(backend).loadChannelMembersPage(
        channel, page, ChannelMemberPageSize,
        [guard, generation, page](QStringList userIds) {
            if (!guard || generation != guard->memberGeneration) {
                return;
            }

            guard->inFlightPages.remove(page);
            const int pageStart = page * ChannelMemberPageSize;
            const ChannelMemberPageDecision decision = channelMemberPageDecision(
                guard->memberCount,
                pageStart,
                ChannelMemberPageSize,
                static_cast<int>(userIds.size()));

            if (!decision.acceptPage) {
                // /stats owns the logical count. A short random page must not
                // collapse a 9k-member channel to pageStart (for example 100).
                // Release LongList's pending suppression so a later user demand
                // can retry the page instead of caching the bad response.
                const int pageLast = pageStart + ChannelMemberPageSize - 1;
                int pendingIndex = 0;
                while (pendingIndex < guard->pendingRanges.size()) {
                    const PendingRange range = guard->pendingRanges.at(pendingIndex);
                    if (range.last < pageStart || range.first > pageLast) {
                        ++pendingIndex;
                        continue;
                    }
                    guard->memberList->finishRangeRequest(range.first, range.last);
                    guard->pendingRanges.removeAt(pendingIndex);
                }
                return;
            }

            const int availableCount = decision.availableCount;
            for (int offset = 0; offset < availableCount; ++offset) {
                guard->memberIds[pageStart + offset] = userIds.at(offset);
            }
            guard->loadedPages.insert(page);

            if (!guard->searchMode && availableCount > 0) {
                guard->memberList->setRangeAvailable(
                    pageStart, pageStart + availableCount - 1, true);
            }

            guard->finishSatisfiedRequests();
        });
}

void ViewChannelMembersListDialog::finishSatisfiedRequests()
{
    if (!memberList) {
        pendingRanges.clear();
        return;
    }

    int index = 0;
    while (index < pendingRanges.size()) {
        const PendingRange range = pendingRanges.at(index);
        bool ready = true;
        for (int logical = range.first; logical <= range.last; ++logical) {
            if (logical >= memberList->itemCount()) {
                continue;
            }
            if (!memberList->isItemAvailable(logical)) {
                ready = false;
                break;
            }
        }

        if (!ready) {
            ++index;
            continue;
        }

        memberList->finishRangeRequest(range.first, range.last);
        pendingRanges.removeAt(index);
    }
}

void ViewChannelMembersListDialog::finishAllRequests()
{
    if (memberList) {
        for (const PendingRange& range : std::as_const(pendingRanges)) {
            memberList->finishRangeRequest(range.first, range.last);
        }
    }
    pendingRanges.clear();
}

void ViewChannelMembersListDialog::reapplyLoadedPages()
{
    if (!memberList || searchMode || memberCount <= 0) {
        return;
    }

    for (int page : std::as_const(loadedPages)) {
        const int first = page * ChannelMemberPageSize;
        if (first >= memberCount) {
            continue;
        }
        int last = first - 1;
        const int pageEnd = std::min(memberCount, first + ChannelMemberPageSize);
        for (int index = first; index < pageEnd; ++index) {
            if (memberIds.at(index).isEmpty()) {
                break;
            }
            last = index;
        }
        if (last >= first) {
            memberList->setRangeAvailable(first, last, true);
        }
    }
}

const BackendUser* ViewChannelMembersListDialog::userAt(int index) const
{
    if (searchMode) {
        return index >= 0 && index < searchUsers.size()
            ? searchUsers.at(index) : nullptr;
    }
    if (index < 0 || index >= memberIds.size() || memberIds.at(index).isEmpty()) {
        return nullptr;
    }
    return backend.getStorage().getUserById(memberIds.at(index));
}

QWidget* ViewChannelMembersListDialog::createMemberRow(int index)
{
    const BackendUser* user = userAt(index);
    QString userId;
    if (user) {
        userId = user->id;
    } else if (!searchMode && index >= 0 && index < memberIds.size()) {
        userId = memberIds.at(index);
    }

    const auto it = channel.members.constFind(userId);
    const BackendChannelMember* membership = it == channel.members.cend()
        ? nullptr : &it.value();
    return new ChannelMemberRow(
        backend, user, userId, channelMemberRole(membership, user),
        [this](const BackendUser* rowUser, const QPoint& globalPos) {
            showMemberContextMenu(rowUser, globalPos);
        });
}

void ViewChannelMembersListDialog::showMemberContextMenu(
    const BackendUser* user,
    const QPoint& globalPos)
{
    if (!user) {
        return;
    }
    QMenu menu(this);
    addContextMenuActions(
        menu, QVariant::fromValue(const_cast<BackendUser*>(user)));
    if (!menu.isEmpty()) {
        menu.exec(globalPos);
    }
}

void ViewChannelMembersListDialog::filterEdited(const QString& text)
{
    searchTimer->stop();
    searchTerm = text.trimmed();
    ++searchGeneration;

    if (searchTerm.isEmpty()) {
        searchUsers.clear();
        searchMode = false;
        restorePagedMembers();
        return;
    }

    QVector<const BackendUser*> localMatches;
    QSet<QString> seen;
    QVector<int> pages = loadedPages.values().toVector();
    std::sort(pages.begin(), pages.end());
    for (int page : std::as_const(pages)) {
        const int first = page * ChannelMemberPageSize;
        const int last = std::min(static_cast<int>(memberIds.size()),
                                  first + ChannelMemberPageSize);
        for (int index = first; index < last; ++index) {
            const QString& userId = memberIds.at(index);
            if (userId.isEmpty() || seen.contains(userId)) {
                continue;
            }
            const BackendUser* user = backend.getStorage().getUserById(userId);
            if (user && matchesUser(*user, searchTerm)) {
                localMatches.push_back(user);
                seen.insert(userId);
            }
        }
    }

    searchMode = true;
    showSearchResults(std::move(localMatches));
    searchTimer->start();
}

void ViewChannelMembersListDialog::performSearch()
{
    if (searchTerm.isEmpty()) {
        return;
    }

    UserSearchOptions options;
    options.term = searchTerm;
    options.inChannelId = channel.id;
    options.allowInactive = true;
    options.limit = MemberSearchLimit;

    const int generation = searchGeneration;
    QPointer<ViewChannelMembersListDialog> guard(this);
    UserProfileService::instance(backend).searchUsers(
        options,
        [guard, generation](QVector<const BackendUser*> users) mutable {
            if (!guard || generation != guard->searchGeneration
                || guard->searchTerm.isEmpty()) {
                return;
            }

            QStringList userIds;
            userIds.reserve(users.size());
            for (const BackendUser* user : std::as_const(users)) {
                if (user) {
                    userIds.push_back(user->id);
                }
            }

            // Presence and uncached channel roles are independent requests.
            // Complete both before displaying results so the role label does
            // not momentarily show Unknown for otherwise valid members.
            auto pending = std::make_shared<int>(2);
            const auto complete =
                [guard, generation, users = std::move(users), pending]() mutable {
                    if (--*pending != 0) {
                        return;
                    }
                    if (guard && generation == guard->searchGeneration
                        && !guard->searchTerm.isEmpty()) {
                        guard->showSearchResults(std::move(users));
                    }
                };
            auto& profiles = UserProfileService::instance(guard->backend);
            profiles.ensureStatuses(userIds, complete);
            profiles.loadChannelMemberRolesByIds(guard->channel, userIds, complete);
        });
}

void ViewChannelMembersListDialog::showSearchResults(
    QVector<const BackendUser*> users)
{
    finishAllRequests();
    memberList->setItemCount(0);
    searchMode = true;
    searchUsers = std::move(users);
    memberList->setItemCount(searchUsers.size());
    if (!searchUsers.isEmpty()) {
        memberList->setRangeAvailable(0, searchUsers.size() - 1, true);
    }
    setItemCountLabel(static_cast<uint32_t>(searchUsers.size()));
}

void ViewChannelMembersListDialog::restorePagedMembers()
{
    finishAllRequests();
    memberList->setItemCount(0);
    memberList->setItemCount(memberCount);
    reapplyLoadedPages();
    setItemCountLabel(static_cast<uint32_t>(memberCount));
}

void ViewChannelMembersListDialog::addContextMenuActions(
    QMenu& menu,
    const QVariant& selectedItemData)
{
    BackendUser* user = selectedItemData.value<BackendUser*>();
    if (!user) {
        return;
    }

    UserListDialog::addContextMenuActions(menu, selectedItemData);

    const auto membership = channel.members.constFind(user->id);
    if (canEditRoles && membership != channel.members.cend()
        && !pendingRoleUpdates.contains(user->id)) {
        const QStringList channelRoles = splitRoleNames(membership.value().roles);
        const QStringList systemRoles = splitRoleNames(
            user->roles.join(QLatin1Char(' ')));
        const bool guest = channelRoles.contains(QStringLiteral("channel_guest"))
            || systemRoles.contains(QStringLiteral("system_guest"));
        const bool admin = membership.value().isAdmin
            || channelRoles.contains(QStringLiteral("channel_admin"));
        if (!guest) {
            QAction* roleAction = menu.addAction(admin
                ? tr("Make Channel Member") : tr("Make Channel Admin"));
            roleAction->setObjectName(admin
                ? QStringLiteral("makeChannelMemberAction")
                : QStringLiteral("makeChannelAdminAction"));
            const QString userId = user->id;
            connect(roleAction, &QAction::triggered, this,
                    [this, userId, admin] { setMemberRole(userId, !admin); });
        }
    }

    menu.addAction(tr("Remove from channel"), [this, user] {
        backend.removeUserFromChannel(channel, user->id);
    });
}

} /* namespace Mattermost */
