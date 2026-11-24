#include <QtTest>

#include <QItemSelectionModel>
#include <QJsonObject>
#include <QPixmap>
#include <QStackedWidget>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelTree.h"
#include "chat-area/ChatArea.h"

using namespace Mattermost;

class ChannelTreeSearchActivationTest : public QObject
{
    Q_OBJECT

private slots:
    void filteredDmAndGmKeepSelectionUntilFilterClears()
    {
        Backend backend;
        Storage& storage = backend.getStorage();
        const QString selfId = QStringLiteral("self-user");
        const QString peerId = QStringLiteral("remote-user");
        BackendUser* self = storage.addUser(QJsonObject {
            {QStringLiteral("id"), selfId},
            {QStringLiteral("username"), QStringLiteral("self")},
        }, true);
        QVERIFY(self);
        BackendUser* peer = storage.addUser(QJsonObject {
            {QStringLiteral("id"), peerId},
            {QStringLiteral("username"), QStringLiteral("searchable-person")},
            {QStringLiteral("first_name"), QStringLiteral("Searchable")},
            {QStringLiteral("last_name"), QStringLiteral("Person")},
        });
        QVERIFY(peer);
        QPixmap avatar(4, 4);
        avatar.fill(Qt::transparent);
        self->avatar = avatar;
        peer->avatar = avatar;

        const QString teamId = QStringLiteral("team-id");
        BackendTeam* team = storage.addTeam(QJsonObject {
            {QStringLiteral("id"), teamId},
            {QStringLiteral("display_name"), QStringLiteral("Test team")},
            {QStringLiteral("name"), QStringLiteral("test-team")},
        });
        QVERIFY(team);
        const QString channelId = QStringLiteral("dm-search-result");
        BackendChannel* channel = storage.addDirectChannel(QJsonObject {
            {QStringLiteral("id"), channelId},
            {QStringLiteral("type"), QStringLiteral("D")},
            {QStringLiteral("name"), selfId + QStringLiteral("__") + peerId},
            {QStringLiteral("create_at"), 1},
        });
        QVERIFY(channel);
        const QString groupId = QStringLiteral("gm-search-result");
        BackendChannel* group = storage.addGroupChannel(QJsonObject {
            {QStringLiteral("id"), groupId},
            {QStringLiteral("type"), QStringLiteral("G")},
            {QStringLiteral("name"), QStringLiteral("searchable-group")},
            {QStringLiteral("display_name"), QStringLiteral("Searchable team group")},
            {QStringLiteral("create_at"), 2},
            {QStringLiteral("last_post_at"), 100},
        });
        QVERIFY(group);

        SidebarCategory dm;
        dm.id = QStringLiteral("dm-category");
        dm.teamId = teamId;
        dm.type = QStringLiteral("direct_messages");
        SidebarTeamState state;
        state.order.append(dm.id);
        state.categories.insert(dm.id, dm);
        SidebarService& sidebar = SidebarService::instance(backend);
        sidebar.applyLocalTeamState(teamId, state);

        ChannelTree tree;
        tree.addTeam(backend, *team);
        tree.setConversationSearchTerm(QStringLiteral("searchable"));

        const auto findRow = [&tree](const QString& id) -> QTreeWidgetItem* {
            for (int i = 0; i < tree.topLevelItemCount(); ++i) {
                QTreeWidgetItem* teamRow = tree.topLevelItem(i);
                for (int j = 0; j < teamRow->childCount(); ++j) {
                    QTreeWidgetItem* categoryRow = teamRow->child(j);
                    for (int k = 0; k < categoryRow->childCount(); ++k) {
                        QTreeWidgetItem* row = categoryRow->child(k);
                        if (row->data(0, ChannelTree::ItemIdRole).toString() == id) {
                            return row;
                        }
                    }
                }
            }
            return nullptr;
        };
        QTreeWidgetItem* dmRow = findRow(channelId);
        QTreeWidgetItem* gmRow = findRow(groupId);
        QVERIFY(dmRow);
        QVERIFY(gmRow);
        const auto visibleSearchIds = [&tree] {
            QStringList ids;
            for (int i = 0; i < tree.topLevelItemCount(); ++i) {
                QTreeWidgetItem* teamRow = tree.topLevelItem(i);
                for (int j = 0; j < teamRow->childCount(); ++j) {
                    QTreeWidgetItem* categoryRow = teamRow->child(j);
                    for (int k = 0; k < categoryRow->childCount(); ++k) {
                        QTreeWidgetItem* row = categoryRow->child(k);
                        if (row->data(0, ChannelTree::ItemKindRole).toInt()
                                == ChannelTree::ChannelItemKind && !row->isHidden()) {
                            ids.push_back(row->data(0, ChannelTree::ItemIdRole)
                                              .toString());
                        }
                    }
                }
            }
            return ids;
        };
        const QStringList initialRows = visibleSearchIds();
        QCOMPARE(initialRows.size(), 2);
        const SidebarCategory* category = sidebar.teamState(teamId)->category(dm.id);
        QVERIFY(!category->channelIds.contains(channelId));
        QVERIFY(!category->channelIds.contains(groupId));

        // Switching between results must not enqueue a category admission,
        // rebuild the tree, or move Qt's highlight to an unrelated row.
        // Use the view's real selection model, not just programmatic
        // navigation: the bug left the ChatArea correct but the highlight wrong.
        tree.setCurrentItem(dmRow, 0, QItemSelectionModel::ClearAndSelect);
        QCOMPARE(tree.currentItem(), dmRow);
        QTest::qWait(20);
        QCOMPARE(tree.currentItem(), dmRow);
        QCOMPARE(tree.selectedItems().size(), 1);
        QCOMPARE(tree.selectedItems().first(), dmRow);
        QVERIFY(!category->channelIds.contains(channelId));
        QVERIFY(dmRow->treeWidget() == &tree);
        QCOMPARE(visibleSearchIds(), initialRows);

        tree.setCurrentItem(gmRow, 0, QItemSelectionModel::ClearAndSelect);
        QCOMPARE(tree.currentItem(), gmRow);
        QTest::qWait(20);
        QCOMPARE(tree.currentItem(), gmRow);
        QCOMPARE(tree.selectedItems().size(), 1);
        QCOMPARE(tree.selectedItems().first(), gmRow);
        QCOMPARE(tree.currentItem()->data(0, ChannelTree::ItemIdRole).toString(),
                 groupId);
        QVERIFY(!category->channelIds.contains(groupId));
        QVERIFY(gmRow->treeWidget() == &tree);
        QCOMPARE(visibleSearchIds(), initialRows);

        // This test deliberately has no ChatArea stack: it checks the real
        // QTreeWidget selection model without initializing chat/network UI.
        // Simulate ChatArea::onActivate() setting the backend's active channel
        // before clearing search, which must admit the currently opened GM.
        backend.setCurrentChannel(*group);
        tree.setConversationSearchTerm(QString());
        QVERIFY(category->channelIds.contains(groupId));
    }

    void sendingOrReceivingInRecentCategoryCannotActivateAnotherRow()
    {
        Backend backend;
        Storage& storage = backend.getStorage();
        QVERIFY(storage.addUser(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("self-user")},
            {QStringLiteral("username"), QStringLiteral("self")},
        }, true));
        BackendTeam* team = storage.addTeam(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("team-id")},
            {QStringLiteral("name"), QStringLiteral("test-team")},
            {QStringLiteral("display_name"), QStringLiteral("Test team")},
        });
        QVERIFY(team);

        const auto addGroup = [&](const QString& id, int timestamp) {
            return storage.addGroupChannel(QJsonObject {
                {QStringLiteral("id"), id},
                {QStringLiteral("type"), QStringLiteral("G")},
                {QStringLiteral("name"), id},
                {QStringLiteral("display_name"), id},
                {QStringLiteral("create_at"), 1},
                {QStringLiteral("last_post_at"), timestamp},
            });
        };
        BackendChannel* first = addGroup(QStringLiteral("gm-first"), 100);
        BackendChannel* second = addGroup(QStringLiteral("gm-second"), 200);
        BackendChannel* third = addGroup(QStringLiteral("gm-third"), 300);
        QVERIFY(first && second && third);

        SidebarCategory directs;
        directs.id = QStringLiteral("dm-category");
        directs.teamId = team->id;
        directs.type = QStringLiteral("direct_messages");
        directs.sorting = QStringLiteral("recent");
        directs.channelIds = QStringList {first->id, second->id, third->id};
        SidebarTeamState state;
        state.order = QStringList {directs.id};
        state.categories.insert(directs.id, directs);
        SidebarService& sidebar = SidebarService::instance(backend);
        sidebar.applyLocalTeamState(team->id, state);

        QStackedWidget stack;
        ChannelTree tree;
        tree.setChatAreaStackedWidget(&stack);
        tree.addTeam(backend, *team);
        // In the real window this is part of startup: it subscribes to
        // SidebarService::categoriesChanged. Without it the test's local
        // category updates never reach the tree.
        tree.populateSidebars(backend);
        sidebar.applyLocalTeamState(team->id, state);

        const auto rowFor = [&tree](const QString& id) -> QTreeWidgetItem* {
            for (int i = 0; i < tree.topLevelItemCount(); ++i) {
                auto* teamRow = tree.topLevelItem(i);
                for (int j = 0; j < teamRow->childCount(); ++j) {
                    auto* category = teamRow->child(j);
                    for (int k = 0; k < category->childCount(); ++k) {
                        auto* item = category->child(k);
                        if (item->data(0, ChannelTree::ItemIdRole).toString() == id)
                            return item;
                    }
                }
            }
            return nullptr;
        };

        QTreeWidgetItem* selected = rowFor(first->id);
        QVERIFY(selected);
        QVERIFY(rowFor(second->id));
        QVERIFY(rowFor(third->id));
        tree.setCurrentItem(selected, 0, QItemSelectionModel::ClearAndSelect);
        QPointer<ChatArea> activePage = tree.getCurrentPage();
        QVERIFY(activePage);
        QCOMPARE(backend.getCurrentChannel(), first);
        QCOMPARE(tree.currentItem(), selected);
        QVERIFY(selected->isSelected());

        QSignalSpy activations(&tree, &ChannelTree::channelActivated);
        int selectionNotifications = 0;
        connect(&tree, &QTreeWidget::currentItemChanged, &tree,
                [&](QTreeWidgetItem*, QTreeWidgetItem*) { ++selectionNotifications; });

        // Own outgoing WebSocket activity promotes the selected conversation
        // from the bottom of Recent to the top. Intermediate Qt indices are
        // not a request to navigate to a neighbouring conversation.
        first->last_post_at = 500;
        sidebar.applyLocalTeamState(team->id, state);
        QCOMPARE(selectionNotifications, 0);
        QCOMPARE(tree.currentItem(), selected);
        QCOMPARE(tree.selectedItems().size(), 1);
        QCOMPARE(tree.selectedItems().first(), selected);
        QCOMPARE(backend.getCurrentChannel(), first);
        QCOMPARE(tree.getCurrentPage(), activePage.data());
        QCOMPARE(selected->parent()->indexOfChild(selected), 0);

        // Incoming activity promotes a different conversation above the
        // selected one. The active conversation and its selection stay put.
        third->last_post_at = 600;
        sidebar.applyLocalTeamState(team->id, state);
        QCOMPARE(selectionNotifications, 0);
        QCOMPARE(tree.currentItem(), selected);
        QCOMPARE(tree.selectedItems().size(), 1);
        QCOMPARE(tree.selectedItems().first(), selected);
        QCOMPARE(backend.getCurrentChannel(), first);

        // Server-side category moves can reparent the selected item; they
        // are equally non-navigational and must retain its identity.
        SidebarCategory favorites;
        favorites.id = QStringLiteral("favorites");
        favorites.teamId = team->id;
        favorites.type = QStringLiteral("favorites");
        favorites.channelIds = QStringList {first->id};
        SidebarTeamState moved = state;
        moved.categories[directs.id].channelIds.removeAll(first->id);
        moved.categories.insert(favorites.id, favorites);
        moved.order.prepend(favorites.id);
        sidebar.applyLocalTeamState(team->id, moved);
        QCOMPARE(selectionNotifications, 0);
        QCOMPARE(tree.currentItem(), selected);
        QCOMPARE(tree.selectedItems().size(), 1);
        QCOMPARE(tree.selectedItems().first(), selected);
        QCOMPARE(selected->parent()->data(0, ChannelTree::ItemIdRole).toString(),
                 favorites.id);

        // Explicit user navigation must still work after all reconciliations.
        QTreeWidgetItem* other = rowFor(second->id);
        QVERIFY(other);
        tree.setCurrentItem(other, 0, QItemSelectionModel::ClearAndSelect);
        QCOMPARE(selectionNotifications, 1);
        QCOMPARE(tree.currentItem(), other);
        // No queued selection rollback may override a subsequent user choice.
        QCoreApplication::processEvents();
        QCOMPARE(selectionNotifications, 1);
        QCOMPARE(tree.currentItem(), other);
        QCOMPARE(activations.count(), 1);
        QCOMPARE(backend.getCurrentChannel(), second);

        // A DM/GM row is a projection. Keep it across search when it belongs
        // to the ordinary sidebar, and keep the live chat even if the server
        // snapshot drops the row while the other page is already materialized.
        tree.openChannel(first->id);
        QCOMPARE(tree.getCurrentPage(), activePage.data());
        state.categories[directs.id].sorting = QStringLiteral("manual");
        sidebar.applyLocalTeamState(team->id, state);
        QCOMPARE(selected->parent()->indexOfChild(selected), 0);

        activations.clear();
        tree.setConversationSearchTerm(QStringLiteral("gm-second"));
        QCOMPARE(rowFor(first->id), selected);
        QCOMPARE(rowFor(second->id), other);
        QVERIFY(selected->isHidden());
        QVERIFY(!other->isHidden());
        QCOMPARE(tree.getCurrentPage(), activePage.data());
        tree.setConversationSearchTerm(QString());
        QCOMPARE(rowFor(first->id), selected);
        QCOMPARE(rowFor(second->id), other);
        QVERIFY(!selected->isHidden());

        SidebarTeamState withoutFirst = state;
        withoutFirst.categories[directs.id].channelIds.removeAll(first->id);
        sidebar.applyLocalTeamState(team->id, withoutFirst);
        QVERIFY(!rowFor(first->id));
        QVERIFY(!tree.currentItem());
        QVERIFY(activePage);
        QCOMPARE(tree.getCurrentPage(), activePage.data());
        QCOMPARE(backend.getCurrentChannel(), first);
        QCOMPARE(activePage->teamContextId(), team->id);
        QCOMPARE(activations.count(), 0);

        tree.openStoredChannel(second->id);
        QCOMPARE(backend.getCurrentChannel(), second);
        tree.openStoredChannel(first->id);
        QCOMPARE(tree.getCurrentPage(), activePage.data());
        QCOMPARE(backend.getCurrentChannel(), first);
        QCOMPARE(activations.count(), 2);

        sidebar.applyLocalTeamState(team->id, state);
        QVERIFY(tree.currentItem());
        QCOMPARE(tree.currentItem(), rowFor(first->id));
        QCOMPARE(rowFor(second->id), other);
        QCOMPARE(tree.getCurrentPage(), activePage.data());
        QCOMPARE(activations.count(), 2);

        // A replacement sidebar is a projection over the same central
        // surface. It must reuse the page, not create a second timeline.
        {
            ChannelTree replacement;
            replacement.setChatAreaStackedWidget(&stack);
            replacement.addTeam(backend, *team);
            const int pagesBefore = stack.count();
            replacement.openStoredChannel(first->id);
            QCOMPARE(replacement.getCurrentPage(), activePage.data());
            QCOMPARE(stack.count(), pagesBefore);
        }
        QVERIFY(activePage);
        tree.setChatAreaStackedWidget(nullptr);

        // Losing channel membership still disposes of its page before the
        // backing BackendChannel can be deleted by Storage, even when no
        // sidebar is attached to the central host.
        emit first->onLeave();
        QVERIFY(activePage.isNull());
        QVERIFY(!rowFor(first->id));
    }
};

QTEST_MAIN(ChannelTreeSearchActivationTest)
#include "ChannelTreeSearchActivationTest.moc"
