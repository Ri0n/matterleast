#include <QtTest>

#include <QTreeWidgetItem>

#include "channel-tree/ChannelTree.h"
#include "channel-tree/ChannelTreeItem.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "channel-tree/team-item/GroupTeamItem.h"
#include <QJsonObject>
#include "channel-tree/channel-item/DirectChannelItem.h"
#include "channel-tree/SidebarItem.h"

using namespace Mattermost;

namespace {

class RecordingMenuItem final : public ChannelTreeItem
{
public:
    explicit RecordingMenuItem(int& calls) : calls_(calls) {}
    void showContextMenu(const QPoint&) override { ++calls_; }

private:
    int& calls_;
};

QTreeWidgetItem* addRow(QTreeWidgetItem* parent, SidebarItem::Kind kind,
                         const QString& remoteId)
{
    auto* row = new QTreeWidgetItem(parent);
    row->setData(0, SidebarItem::KindRole, int(kind));
    row->setData(0, SidebarItem::IdRole, remoteId);
    return row;
}

void verifyBranch(QTreeWidgetItem* row, const QString& accountId)
{
    QVERIFY(row);
    QCOMPARE(row->data(0, SidebarItem::AccountIdRole).toString(), accountId);
    for (int i = 0; i < row->childCount(); ++i)
        verifyBranch(row->child(i), accountId);
}

} // namespace

class ChannelTreeAccountIdentityTest final : public QObject
{
    Q_OBJECT
private slots:
    void categoryDelegatePreservesAccountForMenuAndAction()
    {
        ChannelTree tree;
        tree.resize(420, 180);
        auto* category = new QTreeWidgetItem(&tree);
        category->setText(0, QStringLiteral("Category"));
        category->setData(0, SidebarItem::KindRole, int(SidebarItem::Category));
        category->setData(0, SidebarItem::AccountIdRole, QStringLiteral("server-b"));
        category->setData(0, SidebarItem::TeamIdRole, QStringLiteral("same-team-id"));
        category->setData(0, SidebarItem::IdRole, QStringLiteral("same-category-id"));
        category->setData(0, SidebarItem::CategoryActionRole, true);
        category->setData(0, SidebarItem::CategoryMenuRole, true);
        auto* delegate = qobject_cast<ChannelItemDelegate*>(tree.itemDelegate());
        QVERIFY(delegate);
        QSignalSpy menuSpy(delegate, &ChannelItemDelegate::categoryMenuRequested);
        QSignalSpy actionSpy(delegate, &ChannelItemDelegate::categoryActionRequested);
        tree.show();
        QCoreApplication::processEvents();
        const QRect rect = tree.visualItemRect(category);
        QVERIFY(rect.isValid());
        QTest::mouseClick(tree.viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(rect.right() - 45, rect.center().y()));
        QCOMPARE(menuSpy.count(), 1);
        QCOMPARE(menuSpy.takeFirst(),
                 (QList<QVariant>{QStringLiteral("server-b"),
                                  QStringLiteral("same-team-id"),
                                  QStringLiteral("same-category-id")}));
        QTest::mouseClick(tree.viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(rect.right() - 15, rect.center().y()));
        QCOMPARE(actionSpy.count(), 1);
        QCOMPARE(actionSpy.takeFirst(),
                 (QList<QVariant>{QStringLiteral("server-b"),
                                  QStringLiteral("same-team-id"),
                                  QStringLiteral("same-category-id")}));
    }

    void combinedSearchUpdatesEachBackendWithoutModelReset()
    {
        Backend first;
        Backend second;
        ChannelTree tree;
        QVERIFY(tree.registerAccountBackend(QStringLiteral("a"), first));
        QVERIFY(tree.registerAccountBackend(QStringLiteral("b"), second));
        QSignalSpy resets(tree.model(), &QAbstractItemModel::modelReset);

        tree.setConversationSearchTerm(QStringLiteral("engineering"));
        QVERIFY(SidebarService::instance(first).conversationSearchActive());
        QVERIFY(SidebarService::instance(second).conversationSearchActive());
        tree.setConversationSearchTerm(QString());
        QVERIFY(!SidebarService::instance(first).conversationSearchActive());
        QVERIFY(!SidebarService::instance(second).conversationSearchActive());
        QCOMPARE(resets.count(), 0);
    }

    void backendBindingsAreAccountQualified()
    {
        Backend first;
        Backend second;
        ChannelTree tree;
        auto* firstRow = new QTreeWidgetItem(&tree);
        auto* secondRow = new QTreeWidgetItem(&tree);
        firstRow->setData(0, SidebarItem::AccountIdRole, QStringLiteral("a"));
        secondRow->setData(0, SidebarItem::AccountIdRole, QStringLiteral("b"));
        firstRow->setData(0, SidebarItem::IdRole, QStringLiteral("same-id"));
        secondRow->setData(0, SidebarItem::IdRole, QStringLiteral("same-id"));

        QVERIFY(tree.registerAccountBackend(QStringLiteral("a"), first));
        QVERIFY(tree.registerAccountBackend(QStringLiteral("b"), second));
        QVERIFY(tree.registerAccountBackend(QStringLiteral("a"), first));
        QVERIFY(!tree.registerAccountBackend(QStringLiteral("a"), second));
        QVERIFY(!tree.registerAccountBackend(QStringLiteral("c"), first));
        QVERIFY(tree.backendForAccount(QString()) == nullptr);
        QCOMPARE(tree.backendForRow(firstRow), &first);
        QCOMPARE(tree.backendForRow(secondRow), &second);
        QCOMPARE(tree.accountIdForBackend(&first), QStringLiteral("a"));
        QCOMPARE(tree.accountIdForBackend(&second), QStringLiteral("b"));

        // Removing an account binding cannot redirect its rows to the
        // remaining server, even when the remote channel IDs are identical.
        tree.unregisterAccountBackend(QStringLiteral("a"), &second);
        QCOMPARE(tree.backendForRow(firstRow), &first);
        tree.unregisterAccountBackend(QStringLiteral("a"), &first);
        QVERIFY(tree.backendForRow(firstRow) == nullptr);
        QCOMPARE(tree.backendForRow(secondRow), &second);
        QVERIFY(tree.registerAccountBackend(QStringLiteral("a"), first));
    }

    void singleAccountFallbackBecomesAmbiguousWithSecondBackend()
    {
        Backend first;
        Backend second;
        ChannelTree tree;
        // The original widget still supports unqualified callers as long
        // as exactly one Backend is represented.
        tree.backendForSidebar = &first;
        QVERIFY(tree.bindSingleAccount(QStringLiteral("a")));
        QCOMPARE(tree.backendForAccount(QString()), &first);
        QCOMPARE(tree.backendForAccount(QStringLiteral("a")), &first);
        QCOMPARE(tree.backendInstance(), &first);

        // Adding a server must not silently route an unqualified lookup to
        // the previously bound primary account, even if its ID is unchanged.
        QVERIFY(tree.registerAccountBackend(QStringLiteral("b"), second));
        QVERIFY(tree.backendForAccount(QString()) == nullptr);
        QVERIFY(tree.backendInstance() == nullptr);
        QCOMPARE(tree.backendForAccount(QStringLiteral("a")), &first);
        QCOMPARE(tree.backendForAccount(QStringLiteral("b")), &second);

        // Removing the second Backend restores the legacy single-server
        // behavior without reconstructing or resetting the tree.
        QSignalSpy resets(tree.model(), &QAbstractItemModel::modelReset);
        tree.unregisterAccountBackend(QStringLiteral("b"), &second);
        QCOMPARE(tree.backendForAccount(QString()), &first);
        QCOMPARE(tree.backendInstance(), &first);
        QCOMPARE(resets.count(), 0);
    }


    void accountCatalogKeepsCanonicalChannelsAcrossTeams()
    {
        Backend backend;
        auto& storage = backend.getStorage();
        storage.addUser({{QStringLiteral("id"), QStringLiteral("self")},
                         {QStringLiteral("username"), QStringLiteral("self")}}, true);
        QVERIFY(storage.addTeam({{QStringLiteral("id"), QStringLiteral("team-1")}}));
        QVERIFY(storage.addTeam({{QStringLiteral("id"), QStringLiteral("team-2")}}));
        const QJsonObject dm {
            {QStringLiteral("id"), QStringLiteral("shared-dm")},
            {QStringLiteral("name"), QStringLiteral("self__peer")},
            {QStringLiteral("type"), QStringLiteral("D")}
        };
        auto* first = storage.addOwnChannel(dm);
        QVERIFY(first);
        QCOMPARE(storage.addOwnChannel(dm), first);
        QCOMPARE(storage.directChannels.channels.size(), size_t(1));
        const QJsonObject team {
            {QStringLiteral("id"), QStringLiteral("team-channel")},
            {QStringLiteral("team_id"), QStringLiteral("team-1")},
            {QStringLiteral("type"), QStringLiteral("O")}
        };
        auto* serverChannel = storage.addOwnChannel(team);
        QVERIFY(serverChannel);
        QCOMPARE(storage.addOwnChannel(team), serverChannel);
        QCOMPARE(storage.getTeamById(QStringLiteral("team-1"))->channels.size(),
                 size_t(1));
        QCOMPARE(storage.getTeamById(QStringLiteral("team-2"))->channels.size(),
                 size_t(0));
        QVERIFY(!storage.addOwnChannel({{QStringLiteral("id"), QStringLiteral("orphan")},
                                        {QStringLiteral("type"), QStringLiteral("O")},
                                        {QStringLiteral("team_id"), QStringLiteral("absent")}}));
        // Clearing an old team snapshot must not leave a dangling pointer in
        // the account's global channel index.
        storage.eraseTeam(QStringLiteral("team-1"));
        QVERIFY(!storage.getChannelById(QStringLiteral("team-channel")));
        const QJsonObject replacement {
            {QStringLiteral("id"), QStringLiteral("team-channel")},
            {QStringLiteral("team_id"), QStringLiteral("team-2")},
            {QStringLiteral("type"), QStringLiteral("O")}
        };
        QVERIFY(storage.addOwnChannel(replacement));
        QCOMPARE(storage.getTeamById(QStringLiteral("team-2"))->channels.size(),
                 size_t(1));
    }

    void sharedVisibilityUsesAccountCatalogAndCapsReadHistory()
    {
        Backend backend;
        auto& storage = backend.getStorage();
        storage.addUser({{QStringLiteral("id"), QStringLiteral("self")},
                         {QStringLiteral("username"), QStringLiteral("self")}}, true);

        // No teams or categories: group conversations belong to the account.
        for (int i = 0; i < 45; ++i) {
            const QString id = QStringLiteral("group-%1").arg(i);
            QVERIFY(storage.addOwnChannel({
                {QStringLiteral("id"), id},
                {QStringLiteral("type"), QStringLiteral("G")},
                {QStringLiteral("display_name"),
                 QStringLiteral("self, user-%1").arg(i)},
                {QStringLiteral("last_post_at"), 1000 + i}}));
        }
        const auto& sidebar = SidebarService::instance(backend);
        const QStringList visible = sidebar.visibleDirectConversationIds();
        QCOMPARE(visible.size(), 40);
        QCOMPARE(visible.first(), QStringLiteral("group-44"));
        QCOMPARE(visible.last(), QStringLiteral("group-5"));

        // The self conversation has a Personal row, not a duplicate DM row.
        QVERIFY(storage.addOwnChannel({
            {QStringLiteral("id"), QStringLiteral("self-channel")},
            {QStringLiteral("type"), QStringLiteral("D")},
            {QStringLiteral("name"), QStringLiteral("self__self")}}));
        QVERIFY(!sidebar.visibleDirectConversationIds().contains(
            QStringLiteral("self-channel")));

        // A matching directory search must not be constrained to the 40
        // most recent conversations or to any team's sidebar categories.
        SidebarService::instance(backend).setConversationSearchTerm(
            QStringLiteral("user-0"));
        const QStringList matches = sidebar.visibleDirectConversationIds();
        QCOMPARE(matches.size(), 1);
        QCOMPARE(matches.first(), QStringLiteral("group-0"));
    }

    void mergedViewRejectsUnqualifiedActionsWithOnlyOneBackendOnline()
    {
        Backend first;
        ChannelTree tree;
        tree.backendForSidebar = &first;
        QVERIFY(tree.bindSingleAccount(QStringLiteral("a")));
        QCOMPARE(tree.backendInstance(), &first);
        QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a"), QStringLiteral("b")}));
        QVERIFY(tree.backendForAccount(QString()) == nullptr);
        QVERIFY(tree.backendInstance() == nullptr);
        // Even an explicit API cannot guess a missing account ID in a
        // merged View; only the old singleton slots allow omission.
        auto* row = new QTreeWidgetItem(&tree);
        row->setData(0, SidebarItem::KindRole, int(SidebarItem::Channel));
        row->setData(0, SidebarItem::IdRole, QStringLiteral("sample"));
        row->setData(0, SidebarItem::AccountIdRole, QStringLiteral("a"));
        tree.addChannelToItem(QStringLiteral("sample"), row);
        tree.openChannelForAccount(QStringLiteral("sample"), {});
        QVERIFY(tree.currentItem() == nullptr);
        tree.openStoredChannelForAccount(QStringLiteral("sample"), {});
        QVERIFY(tree.currentItem() == nullptr);
        QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a")}));
        QCOMPARE(tree.backendForAccount(QString()), &first);
        QCOMPARE(tree.backendInstance(), &first);
    }

    void searchOnlyTouchesVisibleAccounts()
    {
        Backend first;
        Backend second;
        ChannelTree tree;
        QVERIFY(tree.registerAccountBackend(QStringLiteral("a"), first));
        QVERIFY(tree.registerAccountBackend(QStringLiteral("b"), second));
        QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a"), QStringLiteral("b")}));
        tree.setConversationSearchTerm(QStringLiteral("foo"));
        QVERIFY(SidebarService::instance(first).conversationSearchActive());
        QVERIFY(SidebarService::instance(second).conversationSearchActive());
        QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a")}));
        tree.setConversationSearchTerm(QString());
        QVERIFY(!SidebarService::instance(first).conversationSearchActive());
        QVERIFY(SidebarService::instance(second).conversationSearchActive());
    }

    void accountRootTransitionsPreserveTeamOrder()
    {
        ChannelTree tree;
        auto addTeam = [&tree](const QString& account, const QString& id) {
            auto* team = new QTreeWidgetItem(&tree);
            team->setData(0, SidebarItem::KindRole, int(SidebarItem::Team));
            team->setData(0, SidebarItem::AccountIdRole, account);
            team->setData(0, SidebarItem::IdRole, id);
            return team;
        };
        QTreeWidgetItem* a1 = addTeam(QStringLiteral("a"), QStringLiteral("a1"));
        QTreeWidgetItem* b1 = addTeam(QStringLiteral("b"), QStringLiteral("b1"));
        QTreeWidgetItem* a2 = addTeam(QStringLiteral("a"), QStringLiteral("a2"));
        QTreeWidgetItem* b2 = addTeam(QStringLiteral("b"), QStringLiteral("b2"));
        QSignalSpy resets(tree.model(), &QAbstractItemModel::modelReset);
        for (int cycle = 0; cycle < 3; ++cycle) {
            QVERIFY(tree.setVisibleAccountRoots(
                {QStringLiteral("a"), QStringLiteral("b")}));
            QCOMPARE(tree.topLevelItem(0)->childCount(), 2);
            QCOMPARE(tree.topLevelItem(1)->childCount(), 2);
            QCOMPARE(tree.topLevelItem(0)->child(0), a1);
            QCOMPARE(tree.topLevelItem(0)->child(1), a2);
            QCOMPARE(tree.topLevelItem(1)->child(0), b1);
            QCOMPARE(tree.topLevelItem(1)->child(1), b2);
            QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a")}));
            QVERIFY(tree.indexOfTopLevelItem(a1) < tree.indexOfTopLevelItem(a2));
            QVERIFY(tree.indexOfTopLevelItem(b1) < tree.indexOfTopLevelItem(b2));
        }
        QCOMPARE(resets.count(), 0);
    }

    void accountRootsPreserveLiveRowsAndSelection()
    {
        ChannelTree tree;
        auto* first = new QTreeWidgetItem(&tree);
        first->setData(0, SidebarItem::KindRole, int(SidebarItem::Team));
        first->setData(0, SidebarItem::AccountIdRole, QStringLiteral("a"));
        first->setData(0, SidebarItem::IdRole, QStringLiteral("shared-team"));
        auto* category = addRow(first, SidebarItem::Category, QStringLiteral("category"));
        auto* channel = addRow(category, SidebarItem::Channel, QStringLiteral("shared-id"));
        auto* second = new QTreeWidgetItem(&tree);
        second->setData(0, SidebarItem::KindRole, int(SidebarItem::Team));
        second->setData(0, SidebarItem::AccountIdRole, QStringLiteral("b"));
        second->setData(0, SidebarItem::IdRole, QStringLiteral("shared-team"));
        auto* secondChannel = addRow(second, SidebarItem::Channel,
                                     QStringLiteral("shared-id"));
        tree.setCurrentItem(channel);
        QAbstractItemModel* const oldModel = tree.model();
        QSignalSpy resets(tree.model(), &QAbstractItemModel::modelReset);
        QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a"), QStringLiteral("b")},
                                            {{QStringLiteral("a"), QStringLiteral("Alpha")},
                                             {QStringLiteral("b"), QStringLiteral("Beta")}}));
        QCOMPARE(tree.model(), oldModel);
        QCOMPARE(tree.topLevelItemCount(), 2);
        auto* aRoot = tree.topLevelItem(0);
        auto* bRoot = tree.topLevelItem(1);
        QCOMPARE(aRoot->data(0, SidebarItem::KindRole).toInt(),
                 int(SidebarItem::AccountRoot));
        QCOMPARE(aRoot->text(0), QStringLiteral("Alpha"));
        QCOMPARE(bRoot->text(0), QStringLiteral("Beta"));
        QCOMPARE(first->parent(), aRoot);
        QCOMPARE(second->parent(), bRoot);
        QCOMPARE(category->parent(), first);
        QCOMPARE(channel->parent(), category);
        QCOMPARE(secondChannel->parent(), second);
        QCOMPARE(tree.currentItem(), channel);
        QCOMPARE(resets.count(), 0);

        QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a"), QStringLiteral("b")},
                                            {{QStringLiteral("a"), QStringLiteral("A")},
                                             {QStringLiteral("b"), QStringLiteral("B")}}));
        QCOMPARE(tree.topLevelItem(0), aRoot);
        QCOMPARE(aRoot->text(0), QStringLiteral("A"));
        QCOMPARE(tree.currentItem(), channel);
        QCOMPARE(resets.count(), 0);

        // A team materialized after the first snapshot goes under its server.
        auto* late = new QTreeWidgetItem(&tree);
        late->setData(0, SidebarItem::KindRole, int(SidebarItem::Team));
        late->setData(0, SidebarItem::AccountIdRole, QStringLiteral("b"));
        tree.attachTeamToAccountRoot(late);
        QCOMPARE(late->parent(), bRoot);

        // A singleton View restores the original team-root hierarchy.
        QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a")}));
        QCOMPARE(tree.topLevelItemCount(), 3);
        QVERIFY(first->parent() == nullptr);
        QVERIFY(second->parent() == nullptr);
        QVERIFY(late->parent() == nullptr);
        QVERIFY(!first->isHidden());
        QVERIFY(second->isHidden());
        QVERIFY(late->isHidden());
        QCOMPARE(tree.currentItem(), channel);
        QCOMPARE(resets.count(), 0);

        // Selecting an account heading before returning to a singleton
        // must not dereference that heading after its deletion.
        QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a"), QStringLiteral("b")}));
        tree.setCurrentItem(tree.topLevelItem(0));
        QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a")}));
        QVERIFY(tree.currentItem() == nullptr);

        // A selection in an account excluded by the next View is cleared.
        QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a"), QStringLiteral("b")}));
        tree.setCurrentItem(secondChannel);
        QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a")}));
        QVERIFY(tree.currentItem() == nullptr);
        QCOMPARE(resets.count(), 0);
    }

    void contextMenusUseOwningAccountWithCollidingIds()
    {
        Backend first;
        Backend second;
        ChannelTree tree;
        tree.backendForSidebar = &first;
        QVERIFY(tree.bindSingleAccount(QStringLiteral("a")));
        QVERIFY(tree.registerAccountBackend(QStringLiteral("b"), second));

        int firstCalls = 0;
        int secondCalls = 0;
        auto* a = new RecordingMenuItem(firstCalls);
        auto* b = new RecordingMenuItem(secondCalls);
        a->setData(0, SidebarItem::KindRole, int(SidebarItem::Channel));
        b->setData(0, SidebarItem::KindRole, int(SidebarItem::Channel));
        a->setData(0, SidebarItem::AccountIdRole, QStringLiteral("a"));
        b->setData(0, SidebarItem::AccountIdRole, QStringLiteral("b"));
        tree.addTopLevelItem(a);
        tree.addTopLevelItem(b);
        tree.addChannelToItem(QStringLiteral("shared-channel"), a);
        tree.addChannelToItem(QStringLiteral("shared-channel"), b);

        tree.showChannelContextMenuForAccount(QStringLiteral("shared-channel"),
                                              QStringLiteral("b"), {});
        QCOMPARE(firstCalls, 0);
        QCOMPARE(secondCalls, 1);
        tree.showChannelContextMenuForAccount(QStringLiteral("shared-channel"),
                                              QStringLiteral("a"), {});
        QCOMPARE(firstCalls, 1);
        QCOMPARE(secondCalls, 1);

        tree.unregisterAccountBackend(QStringLiteral("b"), &second);
        tree.showChannelContextMenuForAccount(QStringLiteral("shared-channel"),
                                              QStringLiteral("b"), {});
        QCOMPARE(secondCalls, 1);
    }

    void sidebarEventsAffectOnlyTheirOriginatingAccount()
    {
        Backend first;
        Backend second;
        ChannelTree tree;
        QVERIFY(tree.registerAccountBackend(QStringLiteral("a"), first));
        QVERIFY(tree.registerAccountBackend(QStringLiteral("b"), second));
        auto* a = new DirectChannelItem(first, nullptr);
        auto* b = new DirectChannelItem(second, nullptr);
        for (const auto& entry : {qMakePair(static_cast<QTreeWidgetItem*>(a), QStringLiteral("a")),
                                  qMakePair(static_cast<QTreeWidgetItem*>(b), QStringLiteral("b"))}) {
            entry.first->setData(0, SidebarItem::KindRole, int(SidebarItem::Channel));
            entry.first->setData(0, SidebarItem::IdRole, QStringLiteral("shared-remote-id"));
            entry.first->setData(0, SidebarItem::AccountIdRole, entry.second);
            tree.addTopLevelItem(entry.first);
            tree.addChannelToItem(QStringLiteral("shared-remote-id"), entry.first);
        }

        tree.populateSidebars(first);
        tree.populateSidebars(second);
        auto& firstSidebar = SidebarService::instance(first);
        auto& secondSidebar = SidebarService::instance(second);
        emit secondSidebar.channelMutedChanged(QStringLiteral("shared-remote-id"), true);
        QVERIFY(!a->data(0, SidebarItem::MutedRole).toBool());
        QVERIFY(b->data(0, SidebarItem::MutedRole).toBool());

        emit firstSidebar.channelMentionedChanged(QStringLiteral("shared-remote-id"), true);
        QVERIFY(a->data(0, SidebarItem::MentionedRole).toBool());
        QVERIFY(!b->data(0, SidebarItem::MentionedRole).toBool());

        emit secondSidebar.channelMutedChanged(QStringLiteral("shared-remote-id"), false);
        QVERIFY(!a->data(0, SidebarItem::MutedRole).toBool());
        QVERIFY(!b->data(0, SidebarItem::MutedRole).toBool());
    }

    void bindsExistingRowsWithoutReplacingTheModel()
    {
        ChannelTree tree;
        auto* team = new QTreeWidgetItem(&tree);
        team->setData(0, SidebarItem::KindRole, int(SidebarItem::Team));
        team->setData(0, SidebarItem::IdRole, QStringLiteral("team"));
        auto* category = addRow(team, SidebarItem::Category,
                                QStringLiteral("category"));
        auto* channel = addRow(category, SidebarItem::Channel,
                               QStringLiteral("channel"));

        QAbstractItemModel* originalModel = tree.model();
        QSignalSpy resets(originalModel, &QAbstractItemModel::modelReset);
        QSignalSpy updates(originalModel, &QAbstractItemModel::dataChanged);
        QVERIFY(tree.bindSingleAccount(QStringLiteral("account-a")));
        QCOMPARE(tree.model(), originalModel);
        QCOMPARE(tree.boundAccountId(), QStringLiteral("account-a"));
        verifyBranch(team, QStringLiteral("account-a"));
        const int applied = updates.count();
        QVERIFY(applied >= 3);
        QCOMPARE(resets.count(), 0);

        // A reentrant login/view activation must neither repaint unchanged
        // account roles nor re-materialize the tested ChannelTree rows.
        QVERIFY(tree.bindSingleAccount(QStringLiteral("account-a")));
        QCOMPARE(updates.count(), applied);
        QCOMPARE(channel, category->child(0));
        QVERIFY(!tree.bindSingleAccount(QStringLiteral("account-b")));
        QCOMPARE(tree.boundAccountId(), QStringLiteral("account-a"));
        verifyBranch(team, QStringLiteral("account-a"));
        QCOMPARE(resets.count(), 0);
    }

    void incomingRowsAndMovedBranchesKeepAccountIdentity()
    {
        ChannelTree tree;
        QVERIFY(tree.bindSingleAccount(QStringLiteral("account-a")));
        QSignalSpy resets(tree.model(), &QAbstractItemModel::modelReset);

        // A server snapshot can insert a subtree that was built off-tree.
        auto* team = new QTreeWidgetItem;
        team->setData(0, SidebarItem::KindRole, int(SidebarItem::Team));
        auto* category = addRow(team, SidebarItem::Category,
                                QStringLiteral("category"));
        addRow(category, SidebarItem::Channel, QStringLiteral("channel"));
        tree.addTopLevelItem(team);
        verifyBranch(team, QStringLiteral("account-a"));

        // New DMs and categories are materialized after the first login.
        auto* added = addRow(category, SidebarItem::Channel,
                             QStringLiteral("new-dm"));
        verifyBranch(added, QStringLiteral("account-a"));

        // Existing reconciliation moves real rows, preserving their metadata
        // and identity instead of reconstructing ChannelItems/ChatAreas.
        auto* another = addRow(team, SidebarItem::Category,
                               QStringLiteral("other"));
        auto* moved = category->takeChild(0);
        another->addChild(moved);
        QCOMPARE(moved->data(0, SidebarItem::IdRole).toString(),
                 QStringLiteral("channel"));
        verifyBranch(moved, QStringLiteral("account-a"));
        verifyBranch(team, QStringLiteral("account-a"));

        // A second account root, prepared off-tree, must never inherit the
        // primary account when it is inserted into the same widget.
        auto* foreign = new QTreeWidgetItem;
        foreign->setData(0, SidebarItem::AccountIdRole, QStringLiteral("account-b"));
        addRow(foreign, SidebarItem::Category, QStringLiteral("shared-category"));
        tree.addTopLevelItem(foreign);
        verifyBranch(foreign, QStringLiteral("account-b"));
        QCOMPARE(resets.count(), 0);
    }

    void liveRowIndexesSurviveBindingAndScopeCollidingChannelIds()
    {
        ChannelTree tree;
        auto* rowA = new QTreeWidgetItem(&tree);
        rowA->setData(0, SidebarItem::KindRole, int(SidebarItem::Channel));
        rowA->setData(0, SidebarItem::IdRole, QStringLiteral("same-channel"));
        tree.addChannelToItem(QStringLiteral("same-channel"), rowA);
        QVERIFY(tree.channelToItemMap.contains({QString(), QStringLiteral("same-channel")}));
        auto* const original = rowA;
        QVERIFY(tree.bindSingleAccount(QStringLiteral("account-a")));
        QCOMPARE(rowA, original);
        QVERIFY(!tree.channelToItemMap.contains({QString(), QStringLiteral("same-channel")}));
        QVERIFY(tree.channelToItemMap.contains(
            {QStringLiteral("account-a"), QStringLiteral("same-channel")}));

        // Before the shared tree is enabled, verify that the same remote ID
        // cannot alias another account's local index. This is the keying
        // contract needed by subsequent multi-backend reconciliation.
        auto* rowB = new QTreeWidgetItem(&tree);
        rowB->setData(0, SidebarItem::KindRole, int(SidebarItem::Channel));
        rowB->setData(0, SidebarItem::IdRole, QStringLiteral("same-channel"));
        rowB->setData(0, SidebarItem::AccountIdRole, QStringLiteral("account-b"));
        tree.addChannelToItem(QStringLiteral("same-channel"), rowB);
        QCOMPARE(tree.channelToItemMap.size(), 2);
        QCOMPARE(tree.channelToItemMap.value(
            {QStringLiteral("account-a"), QStringLiteral("same-channel")}).first(), rowA);
        QCOMPARE(tree.channelToItemMap.value(
            {QStringLiteral("account-b"), QStringLiteral("same-channel")}).first(), rowB);

        tree.removeChannelToItem(QStringLiteral("same-channel"), rowA);
        QCOMPARE(tree.channelToItemMap.size(), 1);
        QCOMPARE(tree.channelToItemMap.value(
            {QStringLiteral("account-b"), QStringLiteral("same-channel")}).first(), rowB);
        tree.removeChannelToItem(QStringLiteral("same-channel"), rowB);
        QVERIFY(tree.channelToItemMap.isEmpty());
    }


    void accountRootsSynchronizeLoadedTeamsWithoutExtraModel()
    {
        Backend a;
        Backend b;
        auto* teamA = a.getStorage().addTeam({
            {QStringLiteral("id"), QStringLiteral("same-team")},
            {QStringLiteral("display_name"), QStringLiteral("Alpha")}});
        auto* teamB = b.getStorage().addTeam({
            {QStringLiteral("id"), QStringLiteral("same-team")},
            {QStringLiteral("display_name"), QStringLiteral("Beta")}});
        QVERIFY(teamA);
        QVERIFY(teamB);
        ChannelTree tree;
        QAbstractItemModel* const originalModel = tree.model();
        QSignalSpy resets(originalModel, &QAbstractItemModel::modelReset);
        QVERIFY(tree.registerAccountBackend(QStringLiteral("a"), a));
        QVERIFY(tree.registerAccountBackend(QStringLiteral("b"), b));
        QVERIFY(tree.setVisibleAccountRoots({QStringLiteral("a"), QStringLiteral("b")}));
        QCOMPARE(tree.topLevelItemCount(), 2);
        QCOMPARE(tree.topLevelItem(0)->childCount(), 1);
        QCOMPARE(tree.topLevelItem(1)->childCount(), 1);
        QTreeWidgetItem* const oldA = tree.topLevelItem(0)->child(0);
        QTreeWidgetItem* const oldB = tree.topLevelItem(1)->child(0);
        teamA->display_name = QStringLiteral("Renamed Alpha");
        emit a.onAllTeamChannelsPopulated();
        QCOMPARE(tree.topLevelItem(0)->child(0), oldA);
        QCOMPARE(tree.topLevelItem(1)->child(0), oldB);
        QCOMPARE(oldA->text(0), QStringLiteral("Renamed Alpha"));
        QCOMPARE(oldB->text(0), QStringLiteral("Beta"));
        QCOMPARE(tree.model(), originalModel);
        QCOMPARE(resets.count(), 0);
    }

    void mergedViewReusesOriginalTreeAndRoutesByAccount()
    {
        Backend first;
        Backend second;
        auto setup = [](Backend& backend, const QString& title) {
            auto& storage = backend.getStorage();
            storage.addUser({{QStringLiteral("id"), QStringLiteral("self")},
                             {QStringLiteral("username"), QStringLiteral("self")}}, true);
            auto* team = storage.addTeam({
                {QStringLiteral("id"), QStringLiteral("shared-team")},
                {QStringLiteral("display_name"), title}});
            QVERIFY(team);
            QVERIFY(storage.addOwnChannel({
                {QStringLiteral("id"), QStringLiteral("same-channel")},
                {QStringLiteral("type"), QStringLiteral("G")},
                {QStringLiteral("display_name"), title + QStringLiteral(" DM")}}));
        };
        setup(first, QStringLiteral("Alpha"));
        setup(second, QStringLiteral("Beta"));

        ChannelTree tree;
        QAbstractItemModel* const originalModel = tree.model();
        QSignalSpy resets(originalModel, &QAbstractItemModel::modelReset);
        QVERIFY(tree.registerAccountBackend(QStringLiteral("a"), first));
        QVERIFY(tree.registerAccountBackend(QStringLiteral("b"), second));
        QVERIFY(tree.setVisibleAccountRoots(
            {QStringLiteral("a"), QStringLiteral("b")}));

        auto* firstTeam = first.getStorage().getTeamById(QStringLiteral("shared-team"));
        auto* secondTeam = second.getStorage().getTeamById(QStringLiteral("shared-team"));
        QVERIFY(firstTeam);
        QVERIFY(secondTeam);
        tree.addTeam(first, *firstTeam);
        tree.addTeam(second, *secondTeam);
        QCOMPARE(tree.topLevelItem(0)->childCount(), 1);
        QCOMPARE(tree.topLevelItem(1)->childCount(), 1);
        auto* firstRow = tree.topLevelItem(0)->child(0);
        auto* secondRow = tree.topLevelItem(1)->child(0);
        QVERIFY(firstRow != secondRow);

        // A repeated team snapshot updates the existing node in place.
        firstTeam->display_name = QStringLiteral("Renamed Alpha");
        tree.addTeam(first, *firstTeam);
        tree.addTeam(second, *secondTeam);
        QCOMPARE(tree.topLevelItem(0)->childCount(), 1);
        QCOMPARE(tree.topLevelItem(1)->childCount(), 1);
        QCOMPARE(tree.topLevelItem(0)->child(0), firstRow);
        QCOMPARE(tree.topLevelItem(1)->child(0), secondRow);
        QCOMPARE(firstRow->text(0), QStringLiteral("Renamed Alpha"));

        // The same remote channel ID must resolve through the account in
        // the original ChannelTree, without creating another chat widget.
        auto addChannelRow = [&tree](QTreeWidgetItem* parent, const QString& account) {
            auto* row = addRow(parent, SidebarItem::Channel,
                               QStringLiteral("same-channel"));
            row->setData(0, SidebarItem::AccountIdRole, account);
            tree.addChannelToItem(QStringLiteral("same-channel"), row);
            return row;
        };
        auto* a = addChannelRow(firstRow, QStringLiteral("a"));
        auto* b = addChannelRow(secondRow, QStringLiteral("b"));
        QSignalSpy opened(&tree, &ChannelTree::conversationOpenRequested);
        tree.openChannelForAccount(QStringLiteral("same-channel"), QStringLiteral("b"));
        QCOMPARE(tree.currentItem(), b);
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.takeFirst(),
                 (QList<QVariant>{QStringLiteral("b"),
                                  QStringLiteral("same-channel")}));
        tree.openChannelForAccount(QStringLiteral("same-channel"), QStringLiteral("a"));
        QCOMPARE(tree.currentItem(), a);
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.takeFirst(),
                 (QList<QVariant>{QStringLiteral("a"),
                                  QStringLiteral("same-channel")}));
        QCOMPARE(tree.model(), originalModel);
        QCOMPARE(resets.count(), 0);
    }

    void identicalRemoteIdsRemainDistinguishableAcrossExistingTrees()
    {
        ChannelTree alpha;
        ChannelTree beta;
        QVERIFY(alpha.bindSingleAccount(QStringLiteral("alpha")));
        QVERIFY(beta.bindSingleAccount(QStringLiteral("beta")));

        auto* first = new QTreeWidgetItem(&alpha);
        auto* second = new QTreeWidgetItem(&beta);
        first->setData(0, SidebarItem::IdRole, QStringLiteral("same-channel"));
        second->setData(0, SidebarItem::IdRole, QStringLiteral("same-channel"));
        first->setData(0, SidebarItem::KindRole, int(SidebarItem::Channel));
        second->setData(0, SidebarItem::KindRole, int(SidebarItem::Channel));

        QCOMPARE(first->data(0, SidebarItem::IdRole),
                 second->data(0, SidebarItem::IdRole));
        QCOMPARE(first->data(0, SidebarItem::AccountIdRole).toString(),
                 QStringLiteral("alpha"));
        QCOMPARE(second->data(0, SidebarItem::AccountIdRole).toString(),
                 QStringLiteral("beta"));
        QCOMPARE(alpha.model()->index(0, 0).data(SidebarItem::AccountIdRole),
                 first->data(0, SidebarItem::AccountIdRole));
        QCOMPARE(beta.model()->index(0, 0).data(SidebarItem::AccountIdRole),
                 second->data(0, SidebarItem::AccountIdRole));
    }
};

QTEST_MAIN(ChannelTreeAccountIdentityTest)
#include "ChannelTreeAccountIdentityTest.moc"
