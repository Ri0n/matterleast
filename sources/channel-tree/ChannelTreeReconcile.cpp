/**
 * @file ChannelTreeReconcile.cpp
 * @brief Incremental reconciliation of the server sidebar with the live tree.
 */

#include "ChannelTree.h"

#include <QPointer>
#include <QSet>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/types/BackendChannel.h"
#include "chat-area/ChatArea.h"
#include "channel-tree/ChannelItem.h"
#include "channel-tree/team-item/TeamItem.h"

namespace Mattermost {
namespace {

QString categoryDisplayName(const SidebarCategory& category)
{
    if (!category.displayName.isEmpty()) {
        return category.displayName;
    }
    if (category.type == QStringLiteral("favorites")) {
        return QStringLiteral("Favorites");
    }
    if (category.type == QStringLiteral("channels")) {
        return QStringLiteral("Channels");
    }
    if (category.type == QStringLiteral("direct_messages")) {
        return QStringLiteral("Direct Messages");
    }
    return QStringLiteral("Category");
}

QTreeWidgetItem* findChannelChild(QTreeWidgetItem* category, const QString& channelId)
{
    if (!category) {
        return nullptr;
    }
    for (int i = 0; i < category->childCount(); ++i) {
        QTreeWidgetItem* row = category->child(i);
        if (row && row->data(0, SidebarItem::KindRole).toInt() == SidebarItem::Channel
            && row->data(0, SidebarItem::IdRole).toString() == channelId) {
            return row;
        }
    }
    return nullptr;
}

QTreeWidgetItem* findVirtualChild(QTreeWidgetItem* category, int destination)
{
    if (!category) {
        return nullptr;
    }
    for (int i = 0; i < category->childCount(); ++i) {
        QTreeWidgetItem* row = category->child(i);
        if (row && row->data(0, SidebarItem::KindRole).toInt() == SidebarItem::VirtualDestination
            && row->data(0, SidebarItem::DestinationRole).toInt() == destination) {
            return row;
        }
    }
    return nullptr;
}

void moveChild(QTreeWidgetItem& parent, QTreeWidgetItem* child, int index)
{
    if (!child || child->parent() != &parent) {
        return;
    }
    const int current = parent.indexOfChild(child);
    if (current < 0 || current == index) {
        return;
    }
    parent.takeChild(current);
    parent.insertChild(index, child);
}

} // namespace

void ChannelTree::verifySidebarTeam(const QString& teamId, quint64 mutation)
{
    if (!backendForSidebar
        || sidebarMutationGeneration.value(teamId) != mutation) {
        return;
    }

    BackendTeam* team = backendForSidebar->getStorage().getTeamById(teamId);
    if (!team) {
        return;
    }

    QPointer<ChannelTree> guard(this);
    SidebarService::instance(*backendForSidebar).retrieveCategories(
        *team,
        [guard, teamId, mutation](const SidebarTeamState& serverState) {
            if (!guard || !guard->backendForSidebar
                || guard->sidebarMutationGeneration.value(teamId) != mutation) {
                return;
            }
            auto& sidebar = SidebarService::instance(*guard->backendForSidebar);
            sidebar.applyLocalTeamState(teamId, serverState);
            if (TeamItem* teamItem = guard->teamToItemMap.value(teamId, nullptr)) {
                guard->reconcileTeamSidebar(*guard->backendForSidebar, *teamItem, serverState);
            }
        },
        {},
        false);
}

void ChannelTree::destroySidebarRow(QTreeWidgetItem* item)
{
    if (!item) {
        return;
    }

    const int kind = item->data(0, ItemKindRole).toInt();
    if (kind == ChannelItemKind) {
        removeChannelToItem(item->data(0, ItemIdRole).toString(), item);
    }

    if (kind == ChannelItemKind || kind == VirtualDestinationItemKind) {
        ChatArea* chatArea = item->data(0, Qt::UserRole).value<ChatArea*>();
        if (chatArea) {
            if (chatAreaStackedWidget) {
                chatAreaStackedWidget->removeWidget(chatArea);
            }
            delete chatArea;
        }
    }
    delete item;
}

void ChannelTree::reconcileTeamSidebar(Backend& backend, TeamItem& teamItem,
                                       const SidebarTeamState& state)
{
    // A server reconciliation is structural. Never leave transient drag geometry
    // attached to rows that may be moved or removed underneath it.
    resetDragVisuals(false);
    renderingSidebar = true;

    auto& sidebar = SidebarService::instance(backend);
    BackendChannel* personalChannel = backend.getStorage().getDirectChannelByUserId(
        backend.getLoginUser().id);

    QMap<QString, QStringList> desiredChannels;
    for (const QString& categoryId : state.order) {
        const SidebarCategory* category = state.category(categoryId);
        if (!category) {
            continue;
        }
        QStringList ids = sidebar.visibleChannelIds(*category);
        if (category->type == QStringLiteral("favorites") && personalChannel) {
            ids.removeAll(personalChannel->id);
        }
        desiredChannels.insert(categoryId, std::move(ids));
    }

    QMap<QString, QTreeWidgetItem*> categories;
    for (int i = 0; i < teamItem.childCount(); ++i) {
        QTreeWidgetItem* category = teamItem.child(i);
        if (!category || category->data(0, ItemKindRole).toInt() != CategoryItemKind) {
            continue;
        }
        categories.insert(category->data(0, ItemIdRole).toString(), category);
    }

    // Rows which no longer belong to their current category are reusable.  Keep
    // them alive so a server-side move preserves the ChannelItem and ChatArea.
    QMap<QString, QList<QTreeWidgetItem*>> movableChannels;
    for (auto it = categories.cbegin(); it != categories.cend(); ++it) {
        QSet<QString> desired;
        const QStringList desiredIds = desiredChannels.value(it.key());
        for (const QString& id : desiredIds) {
            desired.insert(id);
        }
        QTreeWidgetItem* category = it.value();
        for (int row = 0; category && row < category->childCount(); ++row) {
            QTreeWidgetItem* child = category->child(row);
            if (!child || child->data(0, ItemKindRole).toInt() != ChannelItemKind) {
                continue;
            }
            const QString channelId = child->data(0, ItemIdRole).toString();
            if (!desired.contains(channelId)) {
                movableChannels[channelId].push_back(child);
            }
        }
    }

    struct ActiveCategory {
        QTreeWidgetItem* item = nullptr;
        const SidebarCategory* category = nullptr;
        int desiredRows = 0;
    };
    QVector<ActiveCategory> active;

    int categoryIndex = 0;
    QSet<QString> seenCategories;
    for (const QString& categoryId : state.order) {
        const SidebarCategory* category = state.category(categoryId);
        if (!category || seenCategories.contains(categoryId)) {
            continue;
        }
        seenCategories.insert(categoryId);

        QTreeWidgetItem* categoryItem = categories.value(categoryId, nullptr);
        if (!categoryItem) {
            categoryItem = createCategoryItem(
                teamItem, category->id, categoryDisplayName(*category), category->collapsed);
            categories.insert(categoryId, categoryItem);
        }

        const int currentIndex = teamItem.indexOfChild(categoryItem);
        if (currentIndex >= 0 && currentIndex != categoryIndex) {
            teamItem.takeChild(currentIndex);
            teamItem.insertChild(categoryIndex, categoryItem);
        }
        categoryItem->setText(0, categoryDisplayName(*category));
        categoryItem->setData(0, ItemTeamIdRole, teamItem.teamId);
        categoryItem->setExpanded(!category->collapsed);

        active.push_back(ActiveCategory {categoryItem, category, 0});
        ++categoryIndex;
    }

    // Place every desired row first. Surplus rows are only destroyed after all
    // categories had a chance to claim them.
    for (ActiveCategory& entry : active) {
        QTreeWidgetItem* categoryItem = entry.item;
        int rowIndex = 0;
        const bool favorites = entry.category->type == QStringLiteral("favorites");

        if (favorites) {
            QTreeWidgetItem* personal = findVirtualChild(
                categoryItem, SidebarItem::PersonalDestination);
            if (!personal) {
                personal = createPersonalItem(backend, teamItem, *categoryItem);
            }
            moveChild(*categoryItem, personal, rowIndex++);

            QTreeWidgetItem* saved = findVirtualChild(
                categoryItem, SidebarItem::SavedDestination);
            if (!saved) {
                saved = createSavedItem(backend, teamItem, *categoryItem);
            }
            moveChild(*categoryItem, saved, rowIndex++);
        }

        for (const QString& channelId : desiredChannels.value(entry.category->id)) {
            BackendChannel* channel = backend.getStorage().getChannelById(channelId);
            if (!channel) {
                continue;
            }

            QTreeWidgetItem* row = findChannelChild(categoryItem, channelId);
            if (!row) {
                auto& candidates = movableChannels[channelId];
                while (!candidates.isEmpty() && !row) {
                    QTreeWidgetItem* candidate = candidates.takeFirst();
                    if (!candidate || !candidate->parent()) {
                        continue;
                    }
                    QTreeWidgetItem* oldParent = candidate->parent();
                    const int oldIndex = oldParent->indexOfChild(candidate);
                    if (oldIndex >= 0) {
                        oldParent->takeChild(oldIndex);
                        categoryItem->insertChild(rowIndex, candidate);
                        row = candidate;
                    }
                }
            }
            if (!row) {
                row = createChannelItem(backend, teamItem, *categoryItem, *channel);
            }
            moveChild(*categoryItem, row, rowIndex++);
        }
        entry.desiredRows = rowIndex;
    }

    for (const ActiveCategory& entry : active) {
        while (entry.item->childCount() > entry.desiredRows) {
            QTreeWidgetItem* extra = entry.item->takeChild(entry.desiredRows);
            destroySidebarRow(extra);
        }
    }

    // Desired categories were moved to the front in exact order; anything left
    // after them no longer exists in the authoritative state.
    const int activeCategoryCount = static_cast<int>(active.size());
    while (teamItem.childCount() > activeCategoryCount) {
        QTreeWidgetItem* staleCategory = teamItem.takeChild(activeCategoryCount);
        while (staleCategory && staleCategory->childCount() > 0) {
            destroySidebarRow(staleCategory->takeChild(0));
        }
        delete staleCategory;
    }

    teamItem.setExpanded(true);
    renderingSidebar = false;
}

} // namespace Mattermost
