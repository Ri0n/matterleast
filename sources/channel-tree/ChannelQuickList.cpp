/**
 * @file ChannelQuickList.cpp
 * @brief Followed conversation queue used by the Following sidebar tab.
 */

#include "ChannelQuickList.h"

#include <algorithm>
#include <utility>

#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QShowEvent>
#include <QTimer>
#include <QVariant>
#include <QVector>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
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

constexpr int FollowingKeyRole = Qt::UserRole + 100;
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
            emit channelContextMenuRequested(channelId, viewport()->mapToGlobal(pos));
        }
    });
}

void ChannelQuickList::initialize(Backend& backend, Mode)
{
    backend_ = &backend;
    model_ = &FollowingModel::instance(backend);

    connect(model_, &FollowingModel::changed,
            this, &ChannelQuickList::refresh);
    connect(&backend.emojiRegistry(), &EmojiRegistry::customEmojiAdded,
            this, [this](const QString& name) {
        if (!backend_) return;
        for (const QString& id : connectedUsers_) {
            const BackendUser* user = backend_->getStorage().getUserById(id);
            if (user && UserStatusEmoji::name(*user) == name) {
                updateDirectUser(*user);
            }
        }
    });
    refresh();
    model_->ensureThreadsFresh();
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
    QTreeWidgetItem* pressedItem = nullptr;
    if (event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        pressedItem = itemAt(event->position().toPoint());
#else
        pressedItem = itemAt(event->pos());
#endif
    }

    if (event && event->button() == Qt::MiddleButton
        && pressedItem && backend_ && model_) {
        const QString channelId =
            pressedItem->data(0, SidebarItem::ChannelIdRole).toString();
        const QString threadId =
            pressedItem->data(0, SidebarItem::ThreadIdRole).toString();
        const FollowingModel::Entry* entry =
            model_->findEntry(channelId, threadId);
        if (entry || (threadId.isEmpty() && !channelId.isEmpty())) {
            auto& navigation = AppNavigationService::instance(*backend_);
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

void ChannelQuickList::showEvent(QShowEvent* event)
{
    QTreeWidget::showEvent(event);
    if (model_) {
        model_->ensureThreadsFresh();
    }
}

void ChannelQuickList::refreshThreads()
{
    if (model_) {
        model_->ensureThreadsFresh();
    }
}

void ChannelQuickList::activateItem(QTreeWidgetItem* current)
{
    if (refreshing_ || !current || !backend_ || !model_) {
        return;
    }

    const QString key = current->data(0, FollowingKeyRole).toString();
    const bool repeatedActivation =
        !key.isEmpty() && lastActivatedKey_ == key;
    retainedKey_ = key;

    const QString channelId = current->data(0, SidebarItem::ChannelIdRole).toString();
    const QString threadId = current->data(0, SidebarItem::ThreadIdRole).toString();
    // Only treat this row as a Following destination when that source is
    // enabled. The same channel may be present through Channels alone.
    const FollowingModel::Entry* modelEntry = unreadFilters_.following
        ? model_->findEntry(channelId, threadId) : nullptr;
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
        BackendChannel* channel = backend_->getStorage().getChannelById(channelId);
        auto& sidebar = SidebarService::instance(*backend_);
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
        openThread(*entry);
        return;
    }

    BackendChannel* channel = backend_->getStorage().getChannelById(channelId);
    if (!channel) {
        emit channelSelected(channelId);
        return;
    }

    const BackendChannel* currentChannel = backend_->getCurrentChannel();
    if (shouldPreserveRepeatedConversationActivation(
            entry->isThread(),
            entry->channelId,
            currentChannel ? currentChannel->id : QString(),
            repeatedActivation)) {
        AppNavigationService::instance(*backend_).openChannel(channelId);
        return;
    }

    if (!modelEntry) {
        // A Channels-source row has no FollowingModel resume cursor. Resolve
        // the server's unread boundary without inventing model membership.
        QPointer<ChannelQuickList> guard(this);
        const QString requestedKey = key;
        const bool rootsOnly = SidebarService::instance(*backend_)
                                   .usesRootUnreadCounts(*channel);
        backend_->retrieveChannelUnreadPost(
            *channel, [guard, channelId, requestedKey](const QString& postId) {
                if (!guard || !guard->backend_
                    || guard->retainedKey_ != requestedKey
                    || guard->lastActivatedKey_ != requestedKey) {
                    return;
                }
                BackendChannel* currentChannel =
                    guard->backend_->getStorage().getChannelById(channelId);
                if (!currentChannel) return;
                if (!postId.isEmpty()) {
                    AppNavigationService::instance(*guard->backend_).openPost(postId);
                } else {
                    AppNavigationService::instance(*guard->backend_).openChannel(channelId);
                }
            }, rootsOnly);
        return;
    }

    if (entry->resumeState == FollowingModel::ResumeState::FirstUnread
        && !entry->firstUnreadPostId.isEmpty()) {
        AppNavigationService::instance(*backend_).openPost(entry->firstUnreadPostId);
        return;
    }
    if (entry->resumeState == FollowingModel::ResumeState::AtEnd) {
        AppNavigationService::instance(*backend_).openChannel(channelId);
        return;
    }

    if (backend_->getCurrentChannel() == channel && !entry->requiresAttention()) {
        AppNavigationService::instance(*backend_).openChannel(channelId);
        return;
    }

    QPointer<ChannelQuickList> guard(this);
    const QString requestedKey = key;
    backend_->retrieveChannelUnreadPost(
        *channel, [guard, channelId, requestedKey](const QString& postId) {
            if (!guard || !guard->backend_ || !guard->model_
                || guard->retainedKey_ != requestedKey) {
                return;
            }

            const FollowingModel::Entry* current = guard->model_->findEntry(channelId);
            if (!current || current->resumeState == FollowingModel::ResumeState::AtEnd) {
                AppNavigationService::instance(*guard->backend_).openChannel(channelId);
                return;
            }
            if (current->resumeState == FollowingModel::ResumeState::FirstUnread
                && !current->firstUnreadPostId.isEmpty()) {
                AppNavigationService::instance(*guard->backend_).openPost(
                    current->firstUnreadPostId);
                return;
            }

            BackendChannel* currentChannel =
                guard->backend_->getStorage().getChannelById(channelId);
            if (!postId.isEmpty() && currentChannel
                && !isStaleConversationResumeTarget(*current, *currentChannel, postId)) {
                AppNavigationService::instance(*guard->backend_).openPost(postId);
            } else {
                AppNavigationService::instance(*guard->backend_).openChannel(channelId);
            }
        });
}

void ChannelQuickList::releaseSelectionRetention()
{
    if (retainedKey_.isEmpty() && !retainedEntry_) {
        return;
    }

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

QString ChannelQuickList::threadLabel(const FollowingModel::Entry& entry) const
{
    if (!backend_) {
        return compactMessage(entry.message);
    }

    const BackendChannel* channel = backend_->getStorage().getChannelById(entry.channelId);
    const QString channelName = channel ? channel->display_name : entry.channelId;
    const QString snippet = compactMessage(entry.message);
    const QString prefix = entry.synthetic || entry.unreadMentions > 0
        ? QStringLiteral("@ ") : QStringLiteral("\u21aa ");

    return snippet.isEmpty()
        ? prefix + channelName
        : prefix + channelName + QStringLiteral(" \u2014 ") + snippet;
}

void ChannelQuickList::openThread(const FollowingModel::Entry& entry)
{
    if (!backend_ || !model_ || entry.threadId.isEmpty() || entry.channelId.isEmpty()) {
        return;
    }

    if (entry.synthetic) {
        model_->ensureThreadsFresh();
        AppNavigationService::instance(*backend_).openPost(entry.threadId);
        return;
    }

    if (entry.resumeState == FollowingModel::ResumeState::AtEnd) {
        AppNavigationService::instance(*backend_).openThread(entry.channelId, entry.threadId);
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
    AppNavigationService::instance(*backend_).openThreadAtLastViewed(
        entry.channelId, entry.threadId, resumeAfter, fallbackPostId, {}, false);
}

void ChannelQuickList::refresh()
{
    if (!backend_ || !model_) {
        return;
    }

    struct Candidate {
        FollowingModel::Entry entry;
        QString key;
        UnreadSortKey sort;
    };

    QVector<Candidate> candidates;
    QSet<QString> projectedKeys;
    for (const FollowingModel::Entry& entry : model_->entries()) {
        if (!includeFollowingInUnread(entry.isThread(), entry.muted, unreadFilters_)) {
            continue;
        }
        if (entry.isThread() && !backend_->getStorage().getChannelById(entry.channelId)) {
            continue;
        }

        Candidate candidate;
        candidate.entry = entry;
        candidate.key = entryKey(entry);
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
    auto& sidebar = SidebarService::instance(*backend_);
    if (unreadFilters_.channels || unreadFilters_.muted) {
        for (auto it = backend_->getStorage().channels.cbegin();
             it != backend_->getStorage().channels.cend(); ++it) {
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
            const QString key = QStringLiteral("c:") + channel->id;
            if (projectedKeys.contains(key)) continue;

            Candidate candidate;
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

    // A read DM/GM disappears from the shared model immediately. Keep only the
    // selected row in this view so an action never removes the item under the
    // pointer; this retention must not prolong its cursor/model lifetime.
    if (retainedFromFollowing_ && unreadFilters_.following
        && retainedEntry_ && !retainedEntry_->isThread()) {
        const QString key = entryKey(*retainedEntry_);
        bool present = false;
        for (const Candidate& candidate : std::as_const(candidates)) {
            if (candidate.key == key) {
                present = true;
                break;
            }
        }

        BackendChannel* channel = backend_->getStorage().getChannelById(
            retainedEntry_->channelId);
        const bool muted = channel
            ? SidebarService::instance(*backend_).isChannelMuted(*channel)
            : true;
        if (!present && channel && (!muted || unreadFilters_.muted)) {
            Candidate candidate;
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

    refreshing_ = true;
    clear();
    channelItems_.clear();
    QTreeWidgetItem* itemToRestore = nullptr;

    for (const Candidate& candidate : std::as_const(candidates)) {
        const FollowingModel::Entry& entry = candidate.entry;
        auto* item = new QTreeWidgetItem(this);
        item->setData(0, FollowingKeyRole, candidate.key);
        item->setData(0, SidebarItem::UnreadRole, entry.requiresAttention());
        item->setData(0, SidebarItem::MentionedRole,
                      entry.mentioned || entry.unreadMentions > 0);
        item->setData(0, SidebarItem::ChannelIdRole, entry.channelId);
        item->setData(0, SidebarItem::ThreadIdRole, entry.threadId);
        item->setData(0, SidebarItem::TeamIdRole, entry.teamId);
        item->setData(0, SidebarItem::MutedRole, entry.muted);

        if (!entry.isThread()) {
            BackendChannel* channel = backend_->getStorage().getChannelById(entry.channelId);
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
                BackendUser* user = backend_->getStorage().getUserById(channel->name);
                if (user) {
                    if (!user->avatar.isNull()) {
                        item->setIcon(0, QIcon(user->avatar));
                    }
                    item->setData(0, SidebarItem::PresenceRole, user->status);
                    item->setData(0, SidebarItem::CustomStatusEmojiRole,
                                  UserStatusEmoji::pixmap(
                                      backend_->emojiRegistry(), *user, 18, font()));
                    ensureDirectUserConnections(*channel);
                }
            } else {
                item->setIcon(0, ChannelIcons::groupConversation());
            }
            channelItems_.insert(channel->id, item);
        } else {
            BackendChannel* channel = backend_->getStorage().getChannelById(entry.channelId);
            item->setText(0, threadLabel(entry));
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
    refreshing_ = false;
}

void ChannelQuickList::ensureDirectUserConnections(BackendChannel& channel)
{
    if (!backend_ || channel.type != BackendChannel::directChannel) {
        return;
    }

    BackendUser* user = backend_->getStorage().getUserById(channel.name);
    if (!user || connectedUsers_.contains(user->id)) {
        return;
    }

    connectedUsers_.insert(user->id);
    connect(user, &BackendUser::onStatusChanged, this, [this, user] {
        updateDirectUser(*user);
    });
    connect(user, &BackendUser::onCustomStatusChanged, this, [this, user] {
        updateDirectUser(*user);
    });
    connect(user, &BackendUser::onAvatarChanged, this, [this, user] {
        updateDirectUser(*user);
    });
}

void ChannelQuickList::updateDirectUser(const BackendUser& user)
{
    if (!backend_) {
        return;
    }

    BackendChannel* channel = backend_->getStorage().getDirectChannelByUserId(user.id);
    if (!channel) {
        return;
    }

    QTreeWidgetItem* item = channelItems_.value(channel->id, nullptr);
    if (!item) {
        return;
    }

    item->setData(0, SidebarItem::PresenceRole, user.status);
    item->setData(0, SidebarItem::CustomStatusEmojiRole,
                  UserStatusEmoji::pixmap(
                      backend_->emojiRegistry(), user, 18, font()));
    if (!user.avatar.isNull()) {
        item->setIcon(0, QIcon(user.avatar));
    }
    viewport()->update();
}

} // namespace Mattermost
