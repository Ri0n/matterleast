#include <QtTest>

#include <QItemSelectionModel>
#include <QJsonObject>
#include <QPixmap>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelTree.h"

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
};

QTEST_MAIN(ChannelTreeSearchActivationTest)
#include "ChannelTreeSearchActivationTest.moc"
