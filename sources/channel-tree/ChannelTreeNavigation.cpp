#include "ChannelTree.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPointer>
#include <QTreeWidgetItem>

#include "backend/Backend.h"
#include "backend/HTTPConnector.h"
#include "backend/HttpResponseCallback.h"
#include "backend/NetworkRequest.h"
#include "backend/QByteArrayCreator.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendTeam.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/team-item/TeamItem.h"
#include "chat-area/ChatArea.h"

namespace Mattermost {
namespace {

bool containsChannel(const SidebarTeamState& state, const QString& channelId)
{
    for (auto it = state.categories.cbegin(); it != state.categories.cend(); ++it) {
        if (it->channelIds.contains(channelId)) {
            return true;
        }
    }
    return false;
}

} // namespace

void ChannelTree::openStoredChannel(QString channelID)
{
    if (_visibleAccountRootIds.size() > 1)
        return;
    openStoredChannelForAccount(std::move(channelID), _boundAccountId);
}

void ChannelTree::openStoredChannelForAccount(QString channelID, const QString& accountId)
{
    if (accountId.isEmpty() && _visibleAccountRootIds.size() > 1)
        return;
    const ScopedId key {accountId.isEmpty() ? _boundAccountId : accountId, channelID};
    if (sidebarDragActive) {
        pendingStoredChannelOpens.insert(key);
        return;
    }

    auto existing = channelToItemMap.constFind(key);
    if ((existing != channelToItemMap.cend() && !existing.value().isEmpty())
        || hasChannelPage(key.first, channelID)) {
        openChannelForAccount(std::move(channelID), key.first);
        return;
    }

    Backend* owner = backendForAccount(key.first);
    if (!owner) {
        emit storedChannelOpenFinished(key.first, channelID, false);
        return;
    }

    BackendChannel* channel = owner->getStorage().getChannelById(channelID);
    if (!channel) {
        qDebug() << "openStoredChannel" << channelID << ": channel not found in storage";
        emit storedChannelOpenFinished(key.first, channelID, false);
        return;
    }

    // A direct_added event can populate Storage before the server category
    // snapshot catches up. Reconcile that conversation with every loaded
    // Direct Messages category first; in the normal realtime path this also
    // materializes the row immediately.
    if (channel->type == BackendChannel::directChannel
        || channel->type == BackendChannel::groupChannel) {
        admitStoredConversation(*channel);
        existing = channelToItemMap.constFind(key);
        if (existing != channelToItemMap.cend() && !existing.value().isEmpty()) {
            openChannelForAccount(std::move(channelID), key.first);
            return;
        }
    }

    auto& sidebar = SidebarService::instance(*owner);

    // Prefer the Direct Messages category when a channel occurs in more than
    // one server category. A materialized current DM is retained by the sidebar
    // policy on subsequent refreshes even when it is outside the normal limit.
    struct Candidate {
        TeamItem* teamItem = nullptr;
        QTreeWidgetItem* categoryItem = nullptr;
        bool directMessages = false;
    };
    Candidate fallback;

    for (auto teamIt = teamToItemMap.cbegin(); teamIt != teamToItemMap.cend(); ++teamIt) {
        if (teamIt.key().first != key.first)
            continue;
        TeamItem* teamItem = teamIt.value();
        if (!teamItem || &teamItem->backend != owner)
            continue;
        const SidebarTeamState* state = sidebar.teamState(teamIt.key().second);
        if (!teamItem || !state) {
            continue;
        }

        for (const QString& categoryId : state->order) {
            const SidebarCategory* category = state->category(categoryId);
            if (!category || !category->channelIds.contains(channelID)) {
                continue;
            }

            QTreeWidgetItem* categoryItem = nullptr;
            for (int i = 0; i < teamItem->childCount(); ++i) {
                QTreeWidgetItem* child = teamItem->child(i);
                if (child && child->data(0, ItemKindRole).toInt() == CategoryItemKind
                    && child->data(0, ItemIdRole).toString() == categoryId) {
                    categoryItem = child;
                    break;
                }
            }
            if (!categoryItem) {
                continue;
            }

            const bool isDirectMessages = category->type == QStringLiteral("direct_messages");
            if (isDirectMessages) {
                fallback = Candidate {teamItem, categoryItem, true};
                break;
            }
            if (!fallback.categoryItem) {
                fallback = Candidate {teamItem, categoryItem, false};
            }
        }

        if (fallback.directMessages) {
            break;
        }
    }

    if (!fallback.teamItem || !fallback.categoryItem) {
        // The local category snapshot can be stale even for a private channel
        // the user already belongs to. Refresh server categories first for every
        // team channel; only a still-absent public channel is eligible for an
        // automatic join.
        if (!channel->team) {
            qDebug() << "openStoredChannel" << channelID
                     << ": no sidebar category contains channel";
            emit storedChannelOpenFinished(key.first, channelID, false);
            return;
        }
        if (pendingChannelAdmissions.contains(key)) {
            return;
        }

        pendingChannelAdmissions.insert(key);
        const QString teamId = channel->team->id;
        QPointer<ChannelTree> guard(this);
        QPointer<Backend> ownerGuard(owner);
        sidebar.retrieveCategories(*channel->team,
            [guard, ownerGuard, channelID, teamId, key](
                const SidebarTeamState& refreshedState) {
                if (!guard || !ownerGuard
                    || guard->backendForAccount(key.first) != ownerGuard.data())
                    return;

                if (containsChannel(refreshedState, channelID)) {
                    guard->pendingChannelAdmissions.remove(key);
                    guard->openStoredChannelForAccount(channelID, key.first);
                    QTreeWidgetItem* current = guard->currentItem();
                    const bool opened = current
                        && current->data(0, ItemKindRole).toInt() == ChannelItemKind
                        && current->data(0, ItemIdRole).toString() == channelID
                        && current->data(0, ItemAccountIdRole).toString() == key.first;
                    emit guard->storedChannelOpenFinished(key.first, channelID, opened);
                    return;
                }

                BackendChannel* currentChannel =
                    ownerGuard->getStorage().getChannelById(channelID);
                if (!currentChannel || currentChannel->type != BackendChannel::publicChannel) {
                    guard->pendingChannelAdmissions.remove(key);
                    qWarning() << "Channel is absent after sidebar refresh and cannot be auto-joined"
                               << channelID;
                    emit guard->storedChannelOpenFinished(key.first, channelID, false);
                    return;
                }

                // No one-shot network manager per click: the owning Backend
                // already provides the account-scoped HTTP queue and error relay.
                NetworkRequest request(ownerGuard->networkSession(),
                                       QStringLiteral("channels/") + channelID
                                           + QStringLiteral("/members"));
                const QJsonObject payload {
                    {QStringLiteral("user_id"), ownerGuard->getLoginUser().id},
                };
                ownerGuard->httpTransport().post(request, QByteArrayCreator(payload),
                    HttpResponseCallback(
                    [guard, ownerGuard, channelID, teamId, key](
                        QVariant status, const QJsonDocument&) {
                        if (!guard || !ownerGuard
                            || guard->backendForAccount(key.first) != ownerGuard.data())
                            return;

                        if (status.toInt() != QNetworkReply::NoError) {
                            guard->pendingChannelAdmissions.remove(key);
                            qWarning() << "Failed to auto-join public channel" << channelID
                                       << "network status" << status.toInt();
                            emit guard->storedChannelOpenFinished(key.first, channelID, false);
                            return;
                        }

                        BackendTeam* team =
                            ownerGuard->getStorage().getTeamById(teamId);
                        if (!team) {
                            guard->pendingChannelAdmissions.remove(key);
                            qWarning() << "Joined channel" << channelID
                                       << "but team disappeared" << teamId;
                            emit guard->storedChannelOpenFinished(key.first, channelID, false);
                            return;
                        }

                        SidebarService::instance(*ownerGuard.data()).retrieveCategories(
                            *team,
                            [guard, ownerGuard, channelID, key](
                                const SidebarTeamState& joinedState) {
                                if (!guard || !ownerGuard
                                    || guard->backendForAccount(key.first) != ownerGuard.data())
                                    return;
                                guard->pendingChannelAdmissions.remove(key);
                                if (!containsChannel(joinedState, channelID)) {
                                    qWarning() << "Joined channel" << channelID
                                               << "but it is still absent from sidebar categories";
                                    emit guard->storedChannelOpenFinished(key.first, channelID, false);
                                    return;
                                }

                                guard->openStoredChannelForAccount(channelID, key.first);
                                QTreeWidgetItem* current = guard->currentItem();
                                const bool opened = current
                                    && current->data(0, ItemKindRole).toInt() == ChannelItemKind
                                    && current->data(0, ItemIdRole).toString() == channelID
                        && current->data(0, ItemAccountIdRole).toString() == key.first;
                                emit guard->storedChannelOpenFinished(key.first, channelID, opened);
                            });
                    }));
            });
        return;
    }

    ChannelItem* item = createChannelItem(*owner,
                                          fallback.teamItem,
                                          *fallback.categoryItem,
                                          *channel);
    if (!item) {
        emit storedChannelOpenFinished(key.first, channelID, false);
        return;
    }

    fallback.categoryItem->setExpanded(true);
    if (currentItem() == item) {
        activateChannelItem(item);
    } else {
        setCurrentItem(item);
    }
}

} // namespace Mattermost
