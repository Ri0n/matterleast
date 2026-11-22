#include <QtTest>

#include <QTreeWidgetItem>

#include "channel-tree/ChannelTree.h"
#include "channel-tree/SidebarItem.h"

using namespace Mattermost;

namespace {

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
