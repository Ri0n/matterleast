#include <QtTest>

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
    void searchOnlyDmIsAdmittedAfterSelectionNotification()
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

        QTreeWidgetItem* searchRow = nullptr;
        for (int i = 0; i < tree.topLevelItemCount(); ++i) {
            QTreeWidgetItem* teamRow = tree.topLevelItem(i);
            for (int j = 0; j < teamRow->childCount(); ++j) {
                QTreeWidgetItem* categoryRow = teamRow->child(j);
                for (int k = 0; k < categoryRow->childCount(); ++k) {
                    QTreeWidgetItem* row = categoryRow->child(k);
                    if (row->data(0, ChannelTree::ItemIdRole).toString() == channelId) {
                        searchRow = row;
                    }
                }
            }
        }
        QVERIFY(searchRow);
        QVERIFY(!sidebar.teamState(teamId)->category(dm.id)->channelIds.contains(channelId));

        // This used to mutate QTreeWidget from currentItemChanged while Qt
        // was still updating its selection model.
        tree.openChannel(channelId);
        QCOMPARE(tree.currentItem(), searchRow);
        QVERIFY(!sidebar.teamState(teamId)->category(dm.id)->channelIds.contains(channelId));

        QTRY_VERIFY(sidebar.teamState(teamId)->category(dm.id)->channelIds.contains(channelId));
        QCOMPARE(searchRow->treeWidget(), &tree);
    }
};

QTEST_MAIN(ChannelTreeSearchActivationTest)
#include "ChannelTreeSearchActivationTest.moc"
