/**
 * @file AttentionList.cpp
 * @brief Personal attention projection of the shared Following model.
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 */

#include "AttentionList.h"
#include "SidebarQueueRows.h"

#include <algorithm>
#include <utility>

#include <QFont>
#include <QHeaderView>
#include <QIcon>
#include <QMouseEvent>
#include <QPointer>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QTimer>
#include <QVector>

#include "backend/Backend.h"
#include "backend/ChannelUnreadPostSelection.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelIcons.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "ui/UserStatusEmoji.h"
#include "channel-tree/FollowingActivationPolicy.h"
#include "channel-tree/FollowingNavigation.h"
#include "channel-tree/FollowingPresentation.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {
namespace {

constexpr int SelectionSettleDelayMs = 180;

QString entryKey(const FollowingModel::Entry& entry)
{
    return entry.isThread()
        ? QStringLiteral("t:") + entry.threadId
        : QStringLiteral("c:") + entry.channelId;
}

} // namespace

AttentionList::AttentionList(QWidget* parent)
    : QTreeWidget(parent)
    , sources_(this, [this] { refresh(); },
               [this](const QString& id, const BackendUser& user) { updateDirectUser(id, user); })
{
    setColumnCount(1);
    setHeaderHidden(true);
    setRootIsDecorated(false);
    setIndentation(0);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setUniformRowHeights(true);
    setItemDelegate(new ChannelItemDelegate(this));
    header()->setSectionResizeMode(0, QHeaderView::Stretch);

    selectionRefreshTimer_ = new QTimer(this);
    selectionRefreshTimer_->setSingleShot(true);
    selectionRefreshTimer_->setInterval(SelectionSettleDelayMs);
    connect(selectionRefreshTimer_, &QTimer::timeout, this, [this] {
        selectionRefreshTimer_->stop();
        refresh();
    });

    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (refreshing_ || !current) {
            return;
        }

        // Change the navigation cursor immediately, but keep the old geometry
        // stable for a fraction of a second. Any synchronous/asynchronous model
        // updates caused by activation are coalesced by refresh() while the
        // settle timer is active.
        retainSelection(current);
        selectionRefreshTimer_->start();
        activateItem(current);
    });
}

void AttentionList::initialize(Backend& backend)
{
    setAccounts({{SidebarQueueSources::accountId(backend), &backend}});
}

void AttentionList::setAccounts(const SidebarQueueSources::Accounts& accounts)
{
    if (sources_.accounts() == accounts) return;
    if (sources_.backend(retainedAccountId_) != accounts.value(retainedAccountId_)) {
        retainedEntry_.reset();
        retainedAccountId_.clear();
        lastActivatedKey_.clear();
    }
    selectionRefreshTimer_->stop();
    sources_.setAccounts(accounts);
    refresh();
}

void AttentionList::mousePressEvent(QMouseEvent* event)
{
    // A secondary click must not move the attention cursor or open a chat.
    if (event && event->button() == Qt::RightButton) {
        event->accept();
        return;
    }
    QTreeWidgetItem* pressedItem = nullptr;
    if (event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        pressedItem = itemAt(event->position().toPoint());
#else
        pressedItem = itemAt(event->pos());
#endif
    }

    const QString accountId = pressedItem
        ? pressedItem->data(0, SidebarItem::AccountIdRole).toString() : QString();
    Backend* owner = pressedItem ? sources_.backend(accountId) : nullptr;
    if (event && event->button() == Qt::MiddleButton && pressedItem && owner) {
        auto* model = &FollowingModel::instance(*owner);
        const QString channelId =
            pressedItem->data(0, SidebarItem::ChannelIdRole).toString();
        const QString threadId =
            pressedItem->data(0, SidebarItem::ThreadIdRole).toString();
        const FollowingModel::Entry* entry =
            model->findEntry(channelId, threadId);
        if (!entry && retainedEntry_ && retainedAccountId_ == accountId
            && retainedEntry_->channelId == channelId
            && retainedEntry_->threadId == threadId) {
            entry = &*retainedEntry_;
        }
        if (entry) {
            auto& navigation = AppNavigationService::instance(*owner);
            if (entry->isThread()) {
                if (entry->synthetic) {
                    navigation.openPostInTab(entry->threadId);
                } else {
                    navigation.openThreadInTab(entry->channelId, entry->threadId);
                }
            } else {
                navigation.openChannelInTab(entry->channelId);
            }
            event->accept();
            return;
        }
    }

    const bool repeatActivation =
        event && event->button() == Qt::LeftButton
        && pressedItem && pressedItem == currentItem();
    QTreeWidget::mousePressEvent(event);
    if (repeatActivation && pressedItem == currentItem()) {
        activateItem(pressedItem);
    }
}

void AttentionList::mouseReleaseEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::RightButton) {
        event->accept();
        return;
    }
    QTreeWidget::mouseReleaseEvent(event);
}

void AttentionList::retainSelection(QTreeWidgetItem* item)
{
    retainedEntry_.reset();
    retainedAccountId_ = item ? item->data(0, SidebarItem::AccountIdRole).toString() : QString();
    Backend* owner = item ? sources_.backend(retainedAccountId_) : nullptr;
    if (!owner) return;
    const QString channelId = item->data(0, SidebarItem::ChannelIdRole).toString();
    const QString threadId = item->data(0, SidebarItem::ThreadIdRole).toString();
    if (const auto* entry = FollowingModel::instance(*owner).findEntry(channelId, threadId)) {
        retainedEntry_ = *entry;
    }
}

void AttentionList::releaseSelectionRetention()
{
    retainedEntry_.reset();
    retainedAccountId_.clear();
    lastActivatedKey_.clear();
    {
        const QSignalBlocker blocker(this);
        setCurrentItem(nullptr);
        clearSelection();
    }

    // External navigation should have the same visual stability as selecting a
    // neighbouring Attention row: clear selection immediately, compact later.
    selectionRefreshTimer_->start();
}

void AttentionList::refreshThreads()
{
    sources_.refreshThreads();
}

void AttentionList::activateItem(QTreeWidgetItem* item)
{
    if (refreshing_ || !item) return;
    const QString accountId = item->data(0, SidebarItem::AccountIdRole).toString();
    Backend* owner = sources_.backend(accountId);
    if (!owner) return;
    auto* model = &FollowingModel::instance(*owner);
    const QPointer<Backend> sourceGuard(owner);

    const QString channelId = item->data(0, SidebarItem::ChannelIdRole).toString();
    const QString threadId = item->data(0, SidebarItem::ThreadIdRole).toString();
    if (channelId.isEmpty()) {
        return;
    }

    const FollowingModel::Entry* entry = model->findEntry(channelId, threadId);
    const QString key = entry ? SidebarQueueSources::key(accountId, entryKey(*entry)) : QString();
    const bool repeatedActivation =
        !key.isEmpty() && lastActivatedKey_ == key;
    if (!entry) {
        // The row can outlive its attention entry, but its old cursor cannot.
        if (!retainedEntry_ || retainedAccountId_ != accountId
            || retainedEntry_->channelId != channelId
            || retainedEntry_->threadId != threadId) {
            return;
        }

        if (retainedEntry_->isThread()) {
            if (retainedEntry_->synthetic) {
                model->ensureThreadsFresh();
                AppNavigationService::instance(*owner).openPost(threadId);
            } else {
                AppNavigationService::instance(*owner).openThread(channelId, threadId);
            }
        } else {
            AppNavigationService::instance(*owner).openChannel(channelId);
        }
        return;
    }

    lastActivatedKey_ = key;

    if (entry->isThread()) {
        openThread(*owner, *entry);
        return;
    }

    const BackendChannel* currentChannel = owner->getCurrentChannel();
    if (shouldPreserveRepeatedConversationActivation(
            entry->isThread(),
            entry->channelId,
            currentChannel ? currentChannel->id : QString(),
            repeatedActivation)) {
        // Keep the exact viewport on a repeated click, but still animate
        // unread root posts that are already visible in this conversation.
        emit conversationAttentionReactivated(channelId, owner);
        AppNavigationService::instance(*owner).openChannel(channelId);
        return;
    }

    BackendChannel* channel = owner->getStorage().getChannelById(channelId);
    if (!channel) {
        emit channelSelected(channelId, owner);
        return;
    }

    const bool rootsOnly = SidebarService::instance(*owner).usesRootUnreadCounts(*channel);
    // A conversation entry is not a thread entry. A cached FirstUnread cursor
    // can survive a read-mode change and must never send a root-only DM/GM into
    // an old reply's thread (openPost automatically follows root_id).
    const auto validCachedTarget = [channel, rootsOnly](const QString& postId) {
        const BackendPost* post = channel->postIdToPost.value(postId, nullptr);
        return isConversationTargetCompatible(rootsOnly, post != nullptr,
                                              post ? post->root_id : QString());
    };

    if (entry->resumeState == FollowingModel::ResumeState::FirstUnread
        && !entry->firstUnreadPostId.isEmpty()
        && validCachedTarget(entry->firstUnreadPostId)) {
        AppNavigationService::instance(*owner).openPost(entry->firstUnreadPostId);
        return;
    }

    // A normal new root DM may have no FirstUnread cursor yet, and Mattermost's
    // /posts/unread short window may contain only older thread replies. The
    // realtime root is already cached: use that concrete post identity so
    // opening Attention also produces the standard background highlight.
    if (rootsOnly) {
        auto& sidebar = SidebarService::instance(*owner);
        const QString cachedRoot = firstCachedUnreadRootPostId(
            *channel, sidebar.channelLastViewedTime(channelId),
            entry->readThroughCreateAt, entry->readThroughPostId);
        if (!cachedRoot.isEmpty()) {
            AppNavigationService::instance(*owner).openPost(cachedRoot);
            return;
        }
    }

    if (entry->resumeState == FollowingModel::ResumeState::AtEnd) {
        AppNavigationService::instance(*owner).openChannel(channelId);
        return;
    }

    QPointer<AttentionList> guard(this);
    const auto request = AppNavigationService::instance(*owner).beginNavigation();
    owner->retrieveChannelUnreadPost(*channel,
        [guard, sourceGuard, accountId, channelId, rootsOnly, request](const QString& postId) {
            if (!guard || !sourceGuard || !guard->retainedEntry_
                || guard->sources_.backend(accountId) != sourceGuard.data()
                || guard->retainedAccountId_ != accountId
                || !AppNavigationService::instance(*sourceGuard).isNavigationCurrent(request)
                || guard->retainedEntry_->channelId != channelId
                || !guard->retainedEntry_->threadId.isEmpty()) {
                return;
            }

            const FollowingModel::Entry* current = FollowingModel::instance(*sourceGuard).findEntry(channelId);
            if (!current || current->resumeState == FollowingModel::ResumeState::AtEnd) {
                AppNavigationService::instance(*sourceGuard).openChannel(channelId);
                return;
            }
            BackendChannel* currentChannel =
                sourceGuard->getStorage().getChannelById(channelId);
            const bool currentRootsOnly = currentChannel
                && SidebarService::instance(*sourceGuard)
                       .usesRootUnreadCounts(*currentChannel);
            if (current->resumeState == FollowingModel::ResumeState::FirstUnread
                && !current->firstUnreadPostId.isEmpty()) {
                const BackendPost* cached = currentChannel
                    ? currentChannel->postIdToPost.value(current->firstUnreadPostId, nullptr)
                    : nullptr;
                if (isConversationTargetCompatible(
                        currentRootsOnly, cached != nullptr,
                        cached ? cached->root_id : QString())) {
                    AppNavigationService::instance(*sourceGuard).openPost(
                        current->firstUnreadPostId);
                    return;
                }
            }

            // A CRT mode change while the HTTP request was in flight must
            // not turn an unfiltered legacy reply into a parent DM jump.
            const BackendPost* cachedTarget = currentChannel
                ? currentChannel->postIdToPost.value(postId, nullptr) : nullptr;
            const bool verifiedRoot = rootsOnly
                || isConversationTargetCompatible(currentRootsOnly,
                                                  cachedTarget != nullptr,
                                                  cachedTarget
                                                      ? cachedTarget->root_id : QString());
            if (!postId.isEmpty() && currentChannel && verifiedRoot
                && !isStaleConversationResumeTarget(*current, *currentChannel, postId)) {
                AppNavigationService::instance(*sourceGuard).openPost(postId);
            } else {
                // A new root can arrive while the server fallback is running.
                // Prefer that concrete unread identity to a generic channel
                // activation, but never resurrect an older read boundary.
                QString cachedRoot;
                if (currentChannel && currentRootsOnly) {
                    auto& sidebar = SidebarService::instance(*sourceGuard);
                    cachedRoot = firstCachedUnreadRootPostId(
                        *currentChannel, sidebar.channelLastViewedTime(channelId),
                        current->readThroughCreateAt, current->readThroughPostId);
                }
                if (!cachedRoot.isEmpty()) {
                    AppNavigationService::instance(*sourceGuard).openPost(cachedRoot);
                } else {
                    // No verified root target. Keep the channel-level fallback:
                    // following an unrelated thread reply would be incorrect.
                    AppNavigationService::instance(*sourceGuard).openChannel(channelId);
                }
            }
        }, rootsOnly);
}

QString AttentionList::threadLabel(Backend& owner, const FollowingModel::Entry& entry) const
{
    const BackendChannel* channel = owner.getStorage().getChannelById(entry.channelId);
    const QString channelName = channel ? channel->display_name : entry.channelId;
    return followingThreadLabel(channelName, entry.message);
}

void AttentionList::openThread(Backend& owner, const FollowingModel::Entry& entry)
{
    if (entry.threadId.isEmpty()) {
        return;
    }

    if (entry.synthetic) {
        // A click is navigation only. Keep the temporary mention until the
        // target is actually read; channel/thread viewport acknowledgement will
        // reconcile the shared Following model afterwards.
        FollowingModel::instance(owner).ensureThreadsFresh();
        AppNavigationService::instance(owner).openPost(entry.threadId);
        return;
    }

    if (entry.resumeState == FollowingModel::ResumeState::AtEnd) {
        AppNavigationService::instance(owner).openThread(entry.channelId, entry.threadId);
        return;
    }

    uint64_t resumeAfter = entry.lastViewedAt;
    if (entry.hasLocalProgress()) {
        resumeAfter = entry.readThroughCreateAt;
    }
    const QString fallbackPostId =
        entry.resumeState == FollowingModel::ResumeState::FirstUnread
        ? entry.firstUnreadPostId : QString();

    // Attention is a navigation request, not proof that the message was read.
    // Read acknowledgement comes from the thread viewport when its real newest
    // edge has been consumed, preserving the lower-edge rule for tall messages.
    AppNavigationService::instance(owner).openThreadAtLastViewed(
        entry.channelId, entry.threadId, resumeAfter, fallbackPostId, {}, false);
}

void AttentionList::updateDirectUser(const QString& accountId, const BackendUser& user)
{
    auto* owner = sources_.backend(accountId);
    if (!owner || refreshing_) return;
    const auto* channel = owner->getStorage().getDirectChannelByUserId(user.id);
    if (!channel) return;
    for (int i = 0; i < topLevelItemCount(); ++i) {
        auto* item = topLevelItem(i);
        if (item->data(0, SidebarItem::AccountIdRole).toString() != accountId
            || item->data(0, SidebarItem::KindRole).toInt() != SidebarItem::Channel
            || item->data(0, SidebarItem::IdRole).toString() != channel->id)
            continue;
        if (auto* current = owner->getStorage().getUserById(user.id))
            sources_.observeUser(accountId, *current);
        item->setData(0, SidebarItem::PresenceRole, user.status);
        item->setData(0, SidebarItem::CustomStatusEmojiRole,
                      UserStatusEmoji::pixmap(owner->emojiRegistry(), user, 18, font()));
        item->setIcon(0, QIcon(user.avatar));
    }
}

void AttentionList::refresh()
{
    if (refreshing_) return;
    QScopedValueRollback<bool> updating(refreshing_, true);
    struct DisplayEntry {
        QString accountId;
        FollowingModel::Entry entry;
        bool attention = true;
        QString key() const { return SidebarQueueSources::key(accountId, entryKey(entry)); }
    };

    QVector<DisplayEntry> displayEntries;
    const auto accounts = sources_.accounts();
    for (auto it = accounts.cbegin(); it != accounts.cend(); ++it) {
        for (const auto& entry : FollowingModel::instance(*it.value()).entries()) {
            const bool explicitlyFollowedThread = entry.isThread() && !entry.synthetic;
            if (!entry.requiresAttention()
                || !attentionMuteAllowsEntry(entry.muted, explicitlyFollowedThread))
                continue;
            displayEntries.push_back(DisplayEntry {it.key(), entry, true});
        }
    }

    const uint32_t attentionCount = static_cast<uint32_t>(displayEntries.size());
    if (lastAttentionCount_ != static_cast<int>(attentionCount)) {
        lastAttentionCount_ = static_cast<int>(attentionCount);
        emit attentionCountChanged(attentionCount);
    }

    // The existing settle period protects only the selected row's geometry;
    // it must not delay the semantic count or retain rows of a removed account.
    if (selectionRefreshTimer_->isActive()) {
        bool obsoleteRows = false;
        for (int i = 0; i < topLevelItemCount(); ++i)
            if (!sources_.backend(topLevelItem(i)->data(0, SidebarItem::AccountIdRole).toString())) {
                obsoleteRows = true;
                break;
            }
        if (!obsoleteRows) return;
        selectionRefreshTimer_->stop();
    }
    QSignalBlocker blockSelection(this);
    Backend* retainedOwner = sources_.backend(retainedAccountId_);
    if (retainedOwner && retainedEntry_) {
        const QString retainedKey = SidebarQueueSources::key(retainedAccountId_, entryKey(*retainedEntry_));
        bool alreadyPresent = false;
        for (const DisplayEntry& display : std::as_const(displayEntries)) {
            if (display.key() == retainedKey) {
                alreadyPresent = true;
                break;
            }
        }

        if (!alreadyPresent) {
            const FollowingModel::Entry* current = FollowingModel::instance(*retainedOwner).findEntry(
                retainedEntry_->channelId, retainedEntry_->threadId);
            if (current
                && attentionMuteAllowsEntry(
                    current->muted, current->isThread() && !current->synthetic)) {
                displayEntries.push_back(DisplayEntry { retainedAccountId_, *current, false });
            } else if (!retainedEntry_->isThread()) {
                BackendChannel* channel = retainedOwner->getStorage().getChannelById(
                    retainedEntry_->channelId);
                const bool muted = channel
                    ? SidebarService::instance(*retainedOwner).isChannelMuted(*channel)
                    : true;
                if (channel && !muted) {
                    FollowingModel::Entry retained = *retainedEntry_;
                    retained.unread = false;
                    retained.mentioned = false;
                    retained.unreadReplies = 0;
                    retained.unreadMentions = 0;
                    retained.attentionSince = 0;
                    displayEntries.push_back(DisplayEntry { retainedAccountId_, std::move(retained), false });
                }
            } else if (retainedEntry_->synthetic) {
                displayEntries.push_back(DisplayEntry { retainedAccountId_, *retainedEntry_, false });
            }
        }
    }

    std::sort(displayEntries.begin(), displayEntries.end(),
              [](const DisplayEntry& lhs, const DisplayEntry& rhs) {
        const uint64_t lhsTime = lhs.entry.attentionSince != 0
            ? lhs.entry.attentionSince : lhs.entry.lastReplyAt;
        const uint64_t rhsTime = rhs.entry.attentionSince != 0
            ? rhs.entry.attentionSince : rhs.entry.lastReplyAt;
        if (lhsTime != rhsTime) {
            return lhsTime > rhsTime;
        }
        if (lhs.entry.isThread() != rhs.entry.isThread()) {
            return lhs.entry.isThread();
        }
        return lhs.key() < rhs.key();
    });

    const QString selectedKey = retainedEntry_
        ? SidebarQueueSources::key(retainedAccountId_, entryKey(*retainedEntry_)) : QString();
    QSet<QString> desired;
    for (const auto& display : std::as_const(displayEntries)) {
        auto* owner = sources_.backend(display.accountId);
        if (owner && (display.entry.isThread()
            || owner->getStorage().getChannelById(display.entry.channelId)))
            desired.insert(display.key());
    }
    SidebarQueueRows rows(*this, desired);
    QTreeWidgetItem* restoreItem = nullptr;
    int row = 0;

    for (const DisplayEntry& display : std::as_const(displayEntries)) {
        Backend* owner = sources_.backend(display.accountId);
        if (!owner || !desired.contains(display.key())) continue;
        const FollowingModel::Entry& entry = display.entry;
        auto* item = rows.place(display.key(), row++);
        item->setData(0, SidebarItem::AccountIdRole, display.accountId);
        QFont font = item->font(0);
        font.setBold(display.attention);
        font.setUnderline(entry.urgent);
        item->setFont(0, font);
        item->setData(0, SidebarItem::ChannelIdRole, entry.channelId);
        item->setData(0, SidebarItem::ThreadIdRole, entry.threadId);
        item->setData(0, SidebarItem::TeamIdRole, entry.teamId);
        item->setData(0, SidebarItem::UnreadRole, display.attention);
        item->setData(0, SidebarItem::MentionedRole,
                      entry.mentioned || entry.unreadMentions > 0);
        item->setData(0, SidebarItem::MutedRole, entry.muted);

        if (!entry.isThread()) {
            BackendChannel* channel = owner->getStorage().getChannelById(entry.channelId);
            if (!channel) {
                delete item;
                continue;
            }

            item->setText(0, channel->display_name);
            item->setData(0, SidebarItem::KindRole, SidebarItem::Channel);
            item->setData(0, SidebarItem::IdRole, channel->id);
            item->setData(0, SidebarItem::ChannelTypeRole, channel->type);
            item->setToolTip(0, channel->getTeamAndChannelName());

            if (channel->type == BackendChannel::directChannel) {
                BackendUser* user = owner->getStorage().getUserById(channel->name);
                if (user) {
                    item->setData(0, SidebarItem::CustomStatusEmojiRole,
                                  UserStatusEmoji::pixmap(
                                      owner->emojiRegistry(), *user, 18, this->font()));
                    item->setData(0, SidebarItem::PresenceRole, user->status);
                    sources_.observeUser(display.accountId, *user);
                }
                if (!user) {
                    item->setIcon(0, QIcon());
                    item->setData(0, SidebarItem::PresenceRole, QVariant());
                    item->setData(0, SidebarItem::CustomStatusEmojiRole, QVariant());
                    QPointer<AttentionList> guard(this);
                    UserProfileService::instance(*owner).ensureUser(
                        channel->name, [guard](const BackendUser*) {
                            if (guard) {
                                guard->refresh();
                            }
                        });
                } else {
                    item->setIcon(0, QIcon(user->avatar));
                    if (user->avatar.isNull())
                        UserProfileService::instance(*owner).ensureAvatar(*user);
                }
            } else {
                item->setIcon(0, ChannelIcons::conversation(channel->type));
            }
        } else {
            BackendChannel* channel = owner->getStorage().getChannelById(entry.channelId);
            item->setText(0, threadLabel(*owner, entry));
            item->setData(0, SidebarItem::KindRole, SidebarItem::Thread);
            item->setData(0, SidebarItem::IdRole, entry.threadId);
            item->setData(0, SidebarItem::ChannelTypeRole,
                          channel ? channel->type : BackendChannel::publicChannel);
            item->setToolTip(0, followingMessageToolTip(entry.message));
            item->setIcon(0, ChannelIcons::thread());
        }

        if (!selectedKey.isEmpty() && display.key() == selectedKey) {
            restoreItem = item;
        }
    }

    if (restoreItem) {
        setCurrentItem(restoreItem);
    } else {
        setCurrentItem(nullptr);
        clearSelection();
    }
}

} // namespace Mattermost
