#include <QtTest>

#include "workspaces/AccountListModel.h"
#include "workspaces/WorkspaceLayout.h"
#include "workspaces/WorkspaceTreeModel.h"

using namespace Mattermost;

namespace {

WorkspaceLayout topology()
{
    WorkspaceLayout layout;
    layout.accounts = {
        {QStringLiteral("a"), QStringLiteral("Alpha")},
        {QStringLiteral("b"), QStringLiteral("Beta")},
        {QStringLiteral("c"), QStringLiteral("Gamma")}
    };
    layout.profiles = {
        {QStringLiteral("work"), QStringLiteral("Work"),
         {QStringLiteral("a"), QStringLiteral("b")}},
        {QStringLiteral("personal"), QStringLiteral("Personal"),
         {QStringLiteral("c")}}
    };
    layout.views = {
        {QStringLiteral("all"), QStringLiteral("work"), QStringLiteral("All"),
         {QStringLiteral("a"), QStringLiteral("b")}},
        {QStringLiteral("b-only"), QStringLiteral("work"), QStringLiteral("Beta only"),
         {QStringLiteral("b")}},
        {QStringLiteral("home"), QStringLiteral("personal"), QStringLiteral("Home"),
         {QStringLiteral("c")}}
    };
    layout.activeViewId = QStringLiteral("all");
    return layout;
}

AccountListModel::Entry entry(const QString& id, const QString& name,
                              AccountConnectionState state)
{
    return {id, name, QStringLiteral("example.org"), state};
}

} // namespace

class WorkspaceTreeModelTest final : public QObject
{
    Q_OBJECT
private slots:
    void hierarchyUsesStableViewOccurrences()
    {
        WorkspaceLayout layout = topology();
        QVERIFY(layout.validate());
        AccountListModel list;
        list.setEntries({
            entry(QStringLiteral("a"), QStringLiteral("Alpha"),
                  AccountConnectionState::Connected),
            entry(QStringLiteral("b"), QStringLiteral("Beta"),
                  AccountConnectionState::Connecting),
            entry(QStringLiteral("c"), QStringLiteral("Gamma"),
                  AccountConnectionState::Disconnected)
        }, QStringLiteral("a"));
        WorkspaceTreeModel tree(layout, list);

        QCOMPARE(tree.rowCount(), 2);
        const QModelIndex work = tree.index(0, 0);
        const QModelIndex personal = tree.index(1, 0);
        QCOMPARE(tree.data(work, WorkspaceTreeModel::NodeKindRole).toInt(),
                 int(WorkspaceTreeModel::ProfileNode));
        QCOMPARE(tree.data(work, Qt::DisplayRole).toString(), QStringLiteral("Work"));
        QCOMPARE(tree.rowCount(work), 2);
        QCOMPARE(tree.rowCount(personal), 1);

        const QModelIndex all = tree.index(0, 0, work);
        const QModelIndex betaOnly = tree.index(1, 0, work);
        QCOMPARE(tree.data(all, WorkspaceTreeModel::ViewIdRole).toString(),
                 QStringLiteral("all"));
        QVERIFY(tree.data(all, WorkspaceTreeModel::ActiveRole).toBool());
        QVERIFY(!tree.data(betaOnly, WorkspaceTreeModel::ActiveRole).toBool());
        QCOMPARE(tree.parent(betaOnly), work);
        QCOMPARE(tree.rowCount(all), 2);
        QCOMPARE(tree.rowCount(betaOnly), 1);

        const QModelIndex b1 = tree.index(1, 0, all);
        const QModelIndex b2 = tree.index(0, 0, betaOnly);
        QCOMPARE(tree.data(b1, WorkspaceTreeModel::AccountIdRole).toString(),
                 QStringLiteral("b"));
        QCOMPARE(tree.data(b2, WorkspaceTreeModel::AccountIdRole).toString(),
                 QStringLiteral("b"));
        QVERIFY(tree.data(b1, WorkspaceTreeModel::NodeIdRole)
                    != tree.data(b2, WorkspaceTreeModel::NodeIdRole));
        QCOMPARE(tree.data(b1, WorkspaceTreeModel::ConnectionStateRole).toInt(),
                 int(AccountConnectionState::Connecting));
        QCOMPARE(tree.parent(b1), all);
        QCOMPARE(tree.parent(all), work);
        QVERIFY(!tree.parent(work).isValid());
        QCOMPARE(tree.roleNames().value(WorkspaceTreeModel::AccountIdRole),
                 QByteArrayLiteral("accountId"));
    }

    void oneServerStateUpdatesEveryOccurrenceWithoutReset()
    {
        WorkspaceLayout layout = topology();
        AccountListModel list;
        const QVector<AccountListModel::Entry> initial {
            entry(QStringLiteral("a"), QStringLiteral("Alpha"),
                  AccountConnectionState::Connected),
            entry(QStringLiteral("b"), QStringLiteral("Beta"),
                  AccountConnectionState::Connecting),
            entry(QStringLiteral("c"), QStringLiteral("Gamma"),
                  AccountConnectionState::Disconnected)
        };
        list.setEntries(initial, QStringLiteral("a"));
        WorkspaceTreeModel tree(layout, list);
        QSignalSpy resets(&tree, &QAbstractItemModel::modelReset);
        QSignalSpy updates(&tree, &QAbstractItemModel::dataChanged);

        auto changed = initial;
        changed[1].state = AccountConnectionState::Connected;
        list.setEntries(changed, QStringLiteral("a"));
        QCOMPARE(resets.count(), 0);
        QCOMPARE(updates.count(), 2);
        for (const auto& args : updates) {
            const QVector<int> roles = args.at(2).value<QVector<int>>();
            QVERIFY(roles.contains(WorkspaceTreeModel::ConnectionStateRole));
            QVERIFY(roles.contains(WorkspaceTreeModel::StatusRole));
        }
        const QModelIndex work = tree.index(0, 0);
        for (int viewRow = 0; viewRow < 2; ++viewRow) {
            const QModelIndex view = tree.index(viewRow, 0, work);
            const QModelIndex beta = tree.index(viewRow == 0 ? 1 : 0, 0, view);
            QCOMPARE(tree.data(beta, WorkspaceTreeModel::ConnectionStateRole).toInt(),
                     int(AccountConnectionState::Connected));
        }
    }

    void switchingViewUpdatesOnlySelectedViewRoles()
    {
        WorkspaceLayout layout = topology();
        AccountListModel list;
        WorkspaceTreeModel tree(layout, list);
        QSignalSpy resets(&tree, &QAbstractItemModel::modelReset);
        QSignalSpy updates(&tree, &QAbstractItemModel::dataChanged);
        QSignalSpy selected(&tree, &WorkspaceTreeModel::activeViewChanged);

        QVERIFY(layout.selectView(QStringLiteral("b-only")));
        tree.syncActiveView();
        QCOMPARE(tree.activeViewId(), QStringLiteral("b-only"));
        QCOMPARE(resets.count(), 0);
        QCOMPARE(updates.count(), 2);
        QCOMPARE(selected.count(), 1);
        const QModelIndex work = tree.index(0, 0);
        QVERIFY(!tree.data(tree.index(0, 0, work),
                           WorkspaceTreeModel::ActiveRole).toBool());
        QVERIFY(tree.data(tree.index(1, 0, work),
                          WorkspaceTreeModel::ActiveRole).toBool());
        tree.syncActiveView();
        QCOMPARE(updates.count(), 2);
    }

    void topologyRefreshChangesOnlyAffectedNodes()
    {
        WorkspaceLayout layout = topology();
        AccountListModel list;
        WorkspaceTreeModel tree(layout, list);
        QSignalSpy resets(&tree, &QAbstractItemModel::modelReset);
        QSignalSpy updates(&tree, &QAbstractItemModel::dataChanged);
        QSignalSpy inserted(&tree, &QAbstractItemModel::rowsInserted);
        QSignalSpy moved(&tree, &QAbstractItemModel::rowsMoved);
        QSignalSpy removed(&tree, &QAbstractItemModel::rowsRemoved);
        const QModelIndex work = tree.index(0, 0);
        const QModelIndex all = tree.index(0, 0, work);
        QPersistentModelIndex alpha = tree.index(0, 0, all);
        layout.views[1].name = QStringLiteral("Renamed");
        tree.refreshTopology();
        QCOMPARE(resets.count(), 0);
        QCOMPARE(updates.count(), 1);
        QCOMPARE(tree.data(tree.index(1, 0, work),
                           Qt::DisplayRole).toString(), QStringLiteral("Renamed"));
        updates.clear();

        layout.views[0].accountIds.swapItemsAt(0, 1);
        tree.refreshTopology();
        QCOMPARE(moved.count(), 1);
        QCOMPARE(resets.count(), 0);
        QVERIFY(alpha.isValid());
        QCOMPARE(alpha.row(), 1);
        QCOMPARE(alpha.data(WorkspaceTreeModel::AccountIdRole).toString(),
                 QStringLiteral("a"));

        layout.views.append({QStringLiteral("new-view"), QStringLiteral("work"),
                             QStringLiteral("New"), {QStringLiteral("a")}});
        tree.refreshTopology();
        QCOMPARE(inserted.count(), 1);
        QCOMPARE(resets.count(), 0);
        QCOMPARE(tree.rowCount(work), 3);

        layout.views.removeLast();
        tree.refreshTopology();
        QCOMPARE(removed.count(), 1);
        QCOMPARE(resets.count(), 0);
        QVERIFY(alpha.isValid());
        QCOMPARE(alpha.data(WorkspaceTreeModel::AccountIdRole).toString(),
                 QStringLiteral("a"));
    }
};

QTEST_APPLESS_MAIN(WorkspaceTreeModelTest)
#include "WorkspaceTreeModelTest.moc"
