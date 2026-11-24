/**
 * @file ChannelQuickList.cpp
 * @brief Followed conversation queue used by the Following sidebar tab.
 */

#include "ChannelQuickList.h"
#include "SidebarQueueRows.h"

#include <algorithm>
#include <utility>

#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QScopedValueRollback>
#include <QTimer>
#include <QVariant>
#include <QVector>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/UserProfileService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendUser.h"
#include "channel-tree/ChannelIcons.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "channel-tree/FollowingActivationPolicy.h"
#include "channel-tree/FollowingNavigation.h"
#include "channel-tree/FollowingPresentation.h"
#include "channel-tree/SidebarItem.h"
#include "ui/UserStatusEmoji.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {
namespace {

constexpr int FollowingKeyRole = SidebarQueueRows::KeyRole;
constexpr int ThreadSnippetLength = 120;

QString entryKey(const FollowingModel::Entry& entry)
{
    return entry.isThread()
        ? QStringLiteral("t:") + entry.threadId
        : QStringLiteral("c:") + entry.channelId;
}

QString compactMessage(QString message)
{
    message = message.simplified();
    if (message.size() > ThreadSnippetLength) {
        message.truncate(ThreadSnippetLength - 1);
        message += QChar(0x2026);
    }
    return message;
}

} // namespace

ChannelQuickList::ChannelQuickList(QWidget* parent)
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
    setContextMenuPolicy(Qt::CustomContextMenu);
    setItemDelegate(new ChannelItemDelegate(this));
    header()->setSectionResizeMode(0, QHeaderView::Stretch);

    connect(this, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
        if (refreshing_ || retainedKey_.isEmpty()) {
            return;
        }
        const QString currentKey = current
            ? current->data(0, FollowingKeyRole).toString() : QString();
        if (currentKey != retainedKey_) {
            releaseSelectionRetention();
        }
    });

    connect(this, &QTreeWidget::customContextMenuRequested, this,
            [this](const QPoint& pos) {
        QTreeWidgetItem* item = itemAt(pos);
        if (!item) {
            return;
        }
        const QString channelId = item->data(0, SidebarItem::ChannelIdRole).toString();
        if (!channelId.isEmpty()) {
            if (auto* owner = sources_.backend(item->data(0, SidebarItem::AccountIdRole).toString()))
                emit channelContextMenuRequested(channelId, viewport()->mapToGlobal(pos), owner);
        }
    });
}

void ChannelQuickList::initialize(Backend& backend, Mode)
{
    setAccounts({{SidebarQueueSources::accountId(backend), &backend}});
}

void ChannelQuickList::setAccounts(const SidebarQueueSources::Accounts& accounts)
{
    if (sources_.accounts() == accounts) return;
    if (sources_.backend(retainedAccountId_) != accounts.value(retainedAccountId_))
        releaseSelectionRetention();
    sources_.setAccounts(accounts);
    refresh();
}

void ChannelQuickList::setUnreadFilters(const UnreadViewFilters& filters)
{
    if (unreadFilters_.following == filters.following
        && unreadFilters_.channels == filters.channels
        && unreadFilters_.muted == filters.muted) {
        return;
    }
    releaseSelectionRetention();
    unreadFilters_ = filters;
    refresh();
}

void ChannelQuickList::keyPressEvent(QKeyEvent* event)
{
    if (event && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        activateItem(currentItem());
        event->accept();
        return;
    }
    QTreeWidget::keyPressEvent(event);
}

void ChannelQuickList::mousePressEvent(QMouseEvent* event)
{
    // Context menus must not change the selected conversation.
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

    Backend* owner = pressedItem
        ? sources_.backend(pressedItem->data(0, SidebarItem::AccountIdRole).toString()) : nullptr;
    if (event && event->button() == Qt::MiddleButton && pressedItem && owner) {
        auto* model = &FollowingModel::instance(*owner);
        const QString channelId =
            pressedItem->data(0, SidebarItem::ChannelIdRole).toString();
        const QString threadId =
            pressedItem->data(0, SidebarItem::ThreadIdRole).toString();
        const FollowingModel::Entry* entry =
            model->findEntry(channelId, threadId);
        if (entry || (threadId.isEmpty() && !channelId.isEmpty())) {
            auto& navigation = AppNavigationService::instance(*owner);
            if (entry && entry->isThread()) {
                if (entry->synthetic) {
                    navigation.openPostInTab(entry->threadId);
                } else {
                    navigation.openThreadInTab(entry->channelId, entry->threadId);
                }
            } else {
                // Channels-only rows have no FollowingModel entry.
                navigation.openChannelInTab(channelId);
            }
            event->accept();
            return;
        }
    }

    const bool leftActivation = event && event->button() == Qt::LeftButton;
    QTreeWidget::mousePressEvent(event);
    if (leftActivation && pressedItem) {
        activateItem(pressedItem);
    }
}

void ChannelQuickList::mouseReleaseEvent(QMouseEvent* event)
{
    if (event && event->button() == Qt::RightButton) {
        event->accept();
        return;
    }
    QTreeWidget::mouseReleaseEvent(event);
}

void ChannelQuickList::showEvent(QShowEvent* event)
{
    QTreeWidget::showEvent(event);
    sources_.refreshThreads();
}

void ChannelQuickList::refreshThreads()
{
    sources_.refreshThreads();
}

void ChannelQuickList::activateItem(QTreeWidgetItem* current)
{
    if (refreshing_ || !current) return;
    const QString accountId = current->data(0, SidebarItem::AccountIdRole).toString();
    Backend* owner = sources_.backend(accountId);
    if (!owner) return;
    auto* model = &FollowingModel::instance(*owner);
    const QPointer<Backend> sourceGuard(owner);
    const QString key = current->data(0, FollowingKeyRole).toString();
    const bool repeatedActivation =
        !key.isEmpty() && lastActivatedKey_ == key;
    if (retainedKey_ != key) {
        retainedEntry_.reset();
        retainedFromFollowing_ = false;
    }
    retainedKey_ = key;
    retainedAccountId_ = accountId;

    const QString channelId = current->data(0, SidebarItem::ChannelIdRole).toString();
    const QString threadId = current->data(0, SidebarItem::ThreadIdRole).toString();
    // Only treat this row as a Following destination when that source is
    // enabled. The same channel may be present through Channels alone.
    const FollowingModel::Entry* modelEntry = unreadFilters_.following
        ? model->findEntry(channelId, threadId) : nullptr;
    if (modelEntry) {
        retainedEntry_ = *modelEntry;
        retainedFromFollowing_ = true;
    }

    const FollowingModel::Entry* entry = modelEntry;
    if (!entry && retainedEntry_
        && retainedEntry_->channelId == channelId
        && retainedEntry_->threadId == threadId) {
        entry = &*retainedEntry_;
    }
    // Public/private unread channels are projections of SidebarService,
    // not synthetic FollowingModel entries. Keep an ephemeral selection
    // snapshot for click stability, without creating a second read cursor.
    if (!entry && threadId.isEmpty()
        && (unreadFilters_.channels || unreadFilters_.muted)) {
        BackendChannel* channel = owner->getStorage().getChannelById(channelId);
        auto& sidebar = SidebarService::instance(*owner);
        if (channel && includeChannelInUnread(
                channel->type == BackendChannel::publicChannel
                    || channel->type == BackendChannel::privateChannel,
                sidebar.isChannelTracked(channelId),
                sidebar.isChannelUnread(*channel),
                sidebar.isChannelMuted(*channel), unreadFilters_)) {
            FollowingModel::Entry ephemeral;
            ephemeral.kind = FollowingModel::Kind::Conversation;
            ephemeral.channelId = channelId;
            ephemeral.unread = true;
            ephemeral.muted = sidebar.isChannelMuted(*channel);
            ephemeral.lastReplyAt = sidebar.channelActivityTime(*channel);
            retainedEntry_ = std::move(ephemeral);
            retainedFromFollowing_ = false;
            entry = &*retainedEntry_;
        }
    }
    if (!entry) {
        return;
    }

    lastActivatedKey_ = key;

    if (entry->isThread()) {
        openThread(*owner, *entry);
        return;
    }

    BackendChannel* channel = owner->getStorage().getChannelById(channelId);
    if (!channel) {
        emit channelSelected(channelId, owner);
        return;
    }

    const BackendChannel* currentChannel = owner->getCurrentChannel();
    if (shouldPreserveRepeatedConversationActivation(
            entry->isThread(),
            entry->channelId,
            currentChannel ? currentChannel->id : QString(),
            repeatedActivation)) {
        AppNavigationService::instance(*owner).openChannel(channelId);
        return;
    }

    if (!modelEntry) {
        // A Channels-source row has no FollowingModel resume cursor. Resolve
        // the server's unread boundary without inventing model membership.
        QPointer<ChannelQuickList> guard(this);
        const QString requestedKey = key;
        const bool rootsOnly = SidebarService::instance(*owner)
                                   .usesRootUnreadCounts(*channel);
        const auto request = AppNavigationService::instance(*owner).beginNavigation();
        owner->retrieveChannelUnreadPost(
            *channel, [guard, sourceGuard, accountId, channelId, requestedKey, request](const QString& postId) {
                if (!guard || !sourceGuard
                    || guard->sources_.backend(accountId) != sourceGuard.data()
                    || !AppNavigationService::instance(*sourceGuard).isNavigationCurrent(request)
                    || guard->retainedKey_ != requestedKey
                    || guard->lastActivatedKey_ != requestedKey) {
                    return;
                }
                BackendChannel* currentChannel =
                    sourceGuard->getStorage().getChannelById(channelId);
                if (!currentChannel) return;
                if (!postId.isEmpty()) {
                    AppNavigationService::instance(*sourceGuard).openPost(postId);
                } else {
                    AppNavigationService::instance(*sourceGuard).openChannel(channelId);
                }
            }, rootsOnly);
        return;
    }

    if (entry->resumeState == FollowingModel::ResumeState::FirstUnread
        && !entry->firstUnreadPostId.isEmpty()) {
        AppNavigationService::instance(*owner).openPost(entry->firstUnreadPostId);
        return;
    }
    if (entry->resumeState == FollowingModel::ResumeState::AtEnd) {
        AppNavigationService::instance(*owner).openChannel(channelId);
        return;
    }

    if (owner->getCurrentChannel() == channel && !entry->requiresAttention()) {
        AppNavigationService::instance(*owner).openChannel(channelId);
        return;
    }

    QPointer<ChannelQuickList> guard(this);
    const QString requestedKey = key;
    const auto request = AppNavigationService::instance(*owner).beginNavigation();
    owner->retrieveChannelUnreadPost(
        *channel, [guard, sourceGuard, accountId, channelId, requestedKey, request](const QString& postId) {
            if (!guard || !sourceGuard
                || guard->sources_.backend(accountId) != sourceGuard.data()
                || !AppNavigationService::instance(*sourceGuard).isNavigationCurrent(request)
                || guard->retainedKey_ != requestedKey) {
                return;
            }

            const FollowingModel::Entry* current = FollowingModel::instance(*sourceGuard).findEntry(channelId);
            if (!current || current->resumeState == FollowingModel::ResumeState::AtEnd) {
                AppNavigationService::instance(*sourceGuard).openChannel(channelId);
                return;
            }
            if (current->resumeState == FollowingModel::ResumeState::FirstUnread
                && !current->firstUnreadPostId.isEmpty()) {
                AppNavigationService::instance(*sourceGuard).openPost(
                    current->firstUnreadPostId);
                return;
            }

            BackendChannel* currentChannel =
                sourceGuard->getStorage().getChannelById(channelId);
            if (!postId.isEmpty() && currentChannel
                && !isStaleConversationResumeTarget(*current, *currentChannel, postId)) {
                AppNavigationService::instance(*sourceGuard).openPost(postId);
            } else {
                AppNavigationService::instance(*sourceGuard).openChannel(channelId);
            }
        });
}

void ChannelQuickList::releaseSelectionRetention()
{
    if (retainedKey_.isEmpty() && !retainedEntry_) {
        return;
    }

    retainedAccountId_.clear();
    retainedKey_.clear();
    lastActivatedKey_.clear();
    retainedEntry_.reset();
    retainedFromFollowing_ = false;
    QTimer::singleShot(0, this, [this] {
        if (!refreshing_) {
            refresh();
        }
    });
}

QString ChannelQuickList::threadLabel(Backend& owner, const FollowingModel::Entry& entry) const
{
    const BackendChannel* channel = owner.getStorage().getChannelById(entry.channelId);
    const QString channelName = channel ? channel->display_name : entry.channelId;
    const QString snippet = compactMessage(entry.message);
    const QString prefix = entry.synthetic || entry.unreadMentions > 0
        ? QStringLiteral("@ ") : QStringLiteral("\u21aa ");

    return snippet.isEmpty()
        ? prefix + channelName
        : prefix + channelName + QStringLiteral(" \u2014 ") + snippet;
}

void ChannelQuickList::openThread(Backend& owner, const FollowingModel::Entry& entry)
{
    if (entry.threadId.isEmpty() || entry.channelId.isEmpty()) {
        return;
    }

    if (entry.synthetic) {
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

    // Navigation itself is not a read acknowledgement. The thread ChatArea
    // marks it read only when the user has actually reached the newest edge;
    // oversized targets therefore remain unread until their lower edge is seen.
    AppNavigationService::instance(owner).openThreadAtLastViewed(
        entry.channelId, entry.threadId, resumeAfter, fallbackPostId, {}, false);
}

void ChannelQuickList::refresh()
{
    if (refreshing_) return;
    QScopedValueRollback<bool> updating(refreshing_, true);
    QSignalBlocker blockSelection(this);
    struct Candidate {
        QString accountId;
        FollowingModel::Entry entry;
        QString key;
        UnreadSortKey sort;
    };

    QVector<Candidate> candidates;
    QSet<QString> projectedKeys;
    const auto accounts = sources_.accounts();
    for (auto it = accounts.cbegin(); it != accounts.cend(); ++it) {
        const QString accountId = it.key();
        Backend* owner = it.value();
        const auto& model = FollowingModel::instance(*owner);
        for (const FollowingModel::Entry& entry : model.entries()) {
            if (!includeFollowingInUnread(entry.isThread(), entry.muted, unreadFilters_)) {
                continue;
            }
            if (entry.isThread() && !owner->getStorage().getChannelById(entry.channelId)) {
                continue;
            }

            Candidate candidate;
            candidate.accountId = accountId;
            candidate.entry = entry;
            candidate.key = SidebarQueueSources::key(accountId, entryKey(entry));
            const bool unread = entry.requiresAttention();
            const uint64_t sortTime = unread && entry.attentionSince != 0
                ? entry.attentionSince : entry.lastReplyAt;
            candidate.sort = {unreadSortGroup(true, unread), sortTime, candidate.key};
            projectedKeys.insert(candidate.key);
            candidates.push_back(std::move(candidate));
        }

        // Add all unread public/private channels, including channels with
        // outstanding mentions: Following represents mentions as post/thread
        // rows, not necessarily as the channel conversation itself. Joined/
        // tracked membership and unread authority remain in SidebarService.
        // The c:<channelId> key avoids duplicate Following conversation rows.
        auto& sidebar = SidebarService::instance(*owner);
        if (unreadFilters_.channels || unreadFilters_.muted) {
            for (auto it = owner->getStorage().channels.cbegin();
                 it != owner->getStorage().channels.cend(); ++it) {
                BackendChannel* channel = it.value();
                if (!channel || channel->delete_at != 0
                    || !sidebar.isChannelUnread(*channel)) {
                    continue;
                }
                const bool publicOrPrivate =
                    channel->type == BackendChannel::publicChannel
                    || channel->type == BackendChannel::privateChannel;
                const bool muted = sidebar.isChannelMuted(*channel);
                const bool mentioned = sidebar.hasUnreadMention(channel->id);
                // Avoid scanning category membership for ineligible candidates.
                if ((muted && !unreadFilters_.muted)
                    || (!muted && (!unreadFilters_.channels
                                   || !publicOrPrivate))) {
                    continue;
                }
                if (!includeChannelInUnread(publicOrPrivate,
                                            sidebar.isChannelTracked(channel->id),
                                            true, muted, unreadFilters_)) {
                    continue;
                }
                const QString key = SidebarQueueSources::key(accountId, QStringLiteral("c:") + channel->id);
                if (projectedKeys.contains(key)) continue;

                Candidate candidate;
                candidate.accountId = accountId;
                candidate.entry.kind = FollowingModel::Kind::Conversation;
                candidate.entry.channelId = channel->id;
                candidate.entry.teamId = channel->team ? channel->team->id : QString();
                candidate.entry.unread = true;
                candidate.entry.mentioned = mentioned;
                candidate.entry.muted = muted;
                candidate.entry.lastReplyAt = sidebar.channelActivityTime(*channel);
                candidate.key = key;
                candidate.sort = {
                    unreadSortGroup(false, true), candidate.entry.lastReplyAt, key
                };
                projectedKeys.insert(key);
                candidates.push_back(std::move(candidate));
            }
        }
    }

    // A read DM/GM disappears from the shared model immediately. Keep only the
    // selected row in this view so an action never removes the item under the
    // pointer; this retention must not prolong its cursor/model lifetime.
    Backend* retainedOwner = sources_.backend(retainedAccountId_);
    if (retainedOwner && retainedFromFollowing_ && unreadFilters_.following
        && retainedEntry_ && !retainedEntry_->isThread()) {
        const QString key = SidebarQueueSources::key(retainedAccountId_, entryKey(*retainedEntry_));
        bool present = false;
        for (const Candidate& candidate : std::as_const(candidates)) {
            if (candidate.key == key) {
                present = true;
                break;
            }
        }

        BackendChannel* channel = retainedOwner->getStorage().getChannelById(
            retainedEntry_->channelId);
        const bool muted = channel
            ? SidebarService::instance(*retainedOwner).isChannelMuted(*channel)
            : true;
        if (!present && channel && (!muted || unreadFilters_.muted)) {
            Candidate candidate;
            candidate.accountId = retainedAccountId_;
            candidate.entry = *retainedEntry_;
            candidate.entry.unread = false;
            candidate.entry.mentioned = false;
            candidate.entry.unreadReplies = 0;
            candidate.entry.unreadMentions = 0;
            candidate.entry.attentionSince = 0;
            candidate.key = key;
            candidate.sort = {
                unreadSortGroup(true, false), candidate.entry.lastReplyAt, key
            };
            candidates.push_back(std::move(candidate));
        }
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs,
                                                        const Candidate& rhs) {
        return unreadSortBefore(lhs.sort, rhs.sort);
    });

    const QString selectedKey = currentItem()
        ? currentItem()->data(0, FollowingKeyRole).toString() : QString();

    QSet<QString> desired;
    for (const auto& candidate : std::as_const(candidates)) {
        auto* owner = sources_.backend(candidate.accountId);
        if (owner && (candidate.entry.isThread()
            || owner->getStorage().getChannelById(candidate.entry.channelId)))
            desired.insert(candidate.key);
    }
    channelItems_.clear();
    SidebarQueueRows rows(*this, desired);
    QTreeWidgetItem* itemToRestore = nullptr;
    int row = 0;

    for (const Candidate& candidate : std::as_const(candidates)) {
        Backend* owner = sources_.backend(candidate.accountId);
        if (!owner || !desired.contains(candidate.key)) continue;
        const FollowingModel::Entry& entry = candidate.entry;
        auto* item = rows.place(candidate.key, row++);
        item->setData(0, SidebarItem::AccountIdRole, candidate.accountId);
        item->setData(0, FollowingKeyRole, candidate.key);
        item->setData(0, SidebarItem::UnreadRole, entry.requiresAttention());
        item->setData(0, SidebarItem::MentionedRole,
                      entry.mentioned || entry.unreadMentions > 0);
        item->setData(0, SidebarItem::ChannelIdRole, entry.channelId);
        item->setData(0, SidebarItem::ThreadIdRole, entry.threadId);
        item->setData(0, SidebarItem::TeamIdRole, entry.teamId);
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
                    item->setIcon(0, QIcon(user->avatar));
                    item->setData(0, SidebarItem::PresenceRole, user->status);
                    item->setData(0, SidebarItem::CustomStatusEmojiRole,
                                  UserStatusEmoji::pixmap(
                                      owner->emojiRegistry(), *user, 18, font()));
                    sources_.observeUser(candidate.accountId, *user);
                } else {
                    item->setIcon(0, QIcon());
                    item->setData(0, SidebarItem::PresenceRole, QVariant());
                    item->setData(0, SidebarItem::CustomStatusEmojiRole, QVariant());
                }
            } else {
                item->setIcon(0, ChannelIcons::groupConversation());
            }
            channelItems_.insert(candidate.key, item);
        } else {
            BackendChannel* channel = owner->getStorage().getChannelById(entry.channelId);
            item->setText(0, threadLabel(*owner, entry));
            item->setData(0, SidebarItem::KindRole, SidebarItem::Thread);
            item->setData(0, SidebarItem::IdRole, entry.threadId);
            item->setData(0, SidebarItem::ChannelTypeRole,
                          channel ? channel->type : BackendChannel::publicChannel);
            item->setToolTip(0, followingMessageToolTip(entry.message));
            if (channel && channel->type == BackendChannel::privateChannel) {
                item->setIcon(0, ChannelIcons::privateChannel());
            } else {
                item->setIcon(0, ChannelIcons::channel());
            }
        }

        if (candidate.key == selectedKey) {
            itemToRestore = item;
        }
    }

    if (itemToRestore) {
        setCurrentItem(itemToRestore);
    } else {
        setCurrentItem(nullptr);
        clearSelection();
    }
}

void ChannelQuickList::updateDirectUser(const QString& accountId, const BackendUser& user)
{
    auto* owner = sources_.backend(accountId);
    if (!owner || refreshing_) return;

    BackendChannel* channel = owner->getStorage().getDirectChannelByUserId(user.id);
    if (!channel) {
        return;
    }

    QTreeWidgetItem* item = channelItems_.value(
        SidebarQueueSources::key(accountId, QStringLiteral("c:") + channel->id), nullptr);
    if (!item) {
        return;
    }

    if (auto* current = owner->getStorage().getUserById(user.id))
        sources_.observeUser(accountId, *current);
    item->setData(0, SidebarItem::PresenceRole, user.status);
    item->setData(0, SidebarItem::CustomStatusEmojiRole,
                  UserStatusEmoji::pixmap(
                      owner->emojiRegistry(), user, 18, font()));
    item->setIcon(0, QIcon(user.avatar));
    viewport()->update();
}

} // namespace Mattermost
