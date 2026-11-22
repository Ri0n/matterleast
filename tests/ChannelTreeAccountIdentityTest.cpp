#include <QtTest>

#include <QTreeWidgetItem>

#include "channel-tree/ChannelTree.h"
#include "channel-tree/ChannelItemDelegate.h"
#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "channel-tree/channel-item/DirectChannelItem.h"
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
