#include <QtTest>
#include <QAbstractItemModel>
#include <QPushButton>
#include <QTreeView>

#include "backend/AccountManager.h"
#include "ui/ServersSidebarPage.h"
#include "workspaces/WorkspaceTreeModel.h"

using namespace Mattermost;

namespace {

WorkspaceLayout topology()
{
    WorkspaceLayout layout;
    layout.accounts = {
        {QStringLiteral("a"), QStringLiteral("Alpha"),
         QStringLiteral("https://one.example.org"),
         QStringLiteral("remote-a"), QStringLiteral("alpha")},
        {QStringLiteral("b"), QStringLiteral("Beta"),
         QStringLiteral("https://two.example.org"),
         QStringLiteral("remote-b"), QStringLiteral("beta")}
    };
    layout.profiles = {
        {QStringLiteral("both"), QStringLiteral("Simultaneous"),
         {QStringLiteral("a"), QStringLiteral("b")}}
    };
    layout.views = {
        {QStringLiteral("combined"), QStringLiteral("both"),
         QStringLiteral("Combined"), {QStringLiteral("a"), QStringLiteral("b")}},
        {QStringLiteral("second"), QStringLiteral("both"),
         QStringLiteral("Beta"), {QStringLiteral("b")}}
    };
    layout.activeViewId = QStringLiteral("combined");
    return layout;
}

} // namespace

class AccountWorkspaceEditTest final : public QObject
{
    Q_OBJECT
private slots:
    void accountAliasesAndViewOrderUpdateIncrementally()
    {
        AccountManager manager;
        const auto initial = topology();
        QVERIFY(initial.validate());
        QVERIFY(manager.load(initial.toJson()));
        QSignalSpy resets(manager.workspaceTreeModel(),
                          &QAbstractItemModel::modelReset);
        QSignalSpy moved(manager.workspaceTreeModel(),
                         &QAbstractItemModel::rowsMoved);
        QSignalSpy changed(&manager, &AccountManager::layoutChanged);
        auto edited = manager.layout();
        edited.accounts[0].name = QStringLiteral("Work account");
        edited.views[0].accountIds.swapItemsAt(0, 1);
        QString error;
        QVERIFY2(manager.applyLayoutEdits(edited, &error), qPrintable(error));
        QCOMPARE(resets.count(), 0);
        QCOMPARE(moved.count(), 1);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(manager.layout().account(QStringLiteral("a"))->name,
                 QStringLiteral("Work account"));
        const QModelIndex profile = manager.workspaceTreeModel()->index(0, 0);
        const QModelIndex combined = manager.workspaceTreeModel()->index(0, 0, profile);
        const QModelIndex firstServer = manager.workspaceTreeModel()->index(0, 0, combined);
        QCOMPARE(firstServer.data(WorkspaceTreeModel::AccountIdRole).toString(),
                 QStringLiteral("b"));
    }

    void verifiedRemoteAccountIdentityCannotBeEditedFromLayout()
    {
        AccountManager manager;
        QVERIFY(manager.load(topology().toJson()));
        auto edited = manager.layout();
        edited.accounts[0].serverBaseUrl = QStringLiteral("https://unrelated.example.org");
        QString error;
        QSignalSpy changed(&manager, &AccountManager::layoutChanged);
        QVERIFY(!manager.applyLayoutEdits(edited, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(changed.count(), 0);
        QCOMPARE(manager.layout().account(QStringLiteral("a"))->serverBaseUrl,
                 QStringLiteral("https://one.example.org"));
    }

    void invalidViewMembershipDoesNotChangeCatalog()
    {
        AccountManager manager;
        QVERIFY(manager.load(topology().toJson()));
        auto edited = manager.layout();
        edited.views[0].accountIds.clear();
        QString error;
        QSignalSpy resets(manager.workspaceTreeModel(),
                          &QAbstractItemModel::modelReset);
        QVERIFY(!manager.applyLayoutEdits(edited, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(manager.layout().views[0].accountIds.size(), 2);
        QCOMPARE(resets.count(), 0);
    }

    void serversPageUsesSharedTreeAndProvidesLayoutControls()
    {
        AccountManager manager;
        QVERIFY(manager.load(topology().toJson()));
        ServersSidebarPage page(manager);
        auto* tree = page.findChild<QTreeView*>(QStringLiteral("serversWorkspaceTree"));
        QVERIFY(tree);
        QCOMPARE(tree->model(), manager.workspaceTreeModel());
        QCOMPARE(tree->model()->rowCount(), 1);
        auto* layouts = page.findChild<QPushButton*>(QStringLiteral("serversEditLayout"));
        auto* add = page.findChild<QPushButton*>(QStringLiteral("serversAdd"));
        auto* open = page.findChild<QPushButton*>(QStringLiteral("serversOpen"));
        QVERIFY(layouts);
        QVERIFY(add);
        QVERIFY(open);
        QVERIFY(!open->isEnabled());
        QSignalSpy addRequested(&page, &ServersSidebarPage::addServerRequested);
        add->click();
        QCOMPARE(addRequested.count(), 1);
    }
};

QTEST_MAIN(AccountWorkspaceEditTest)
#include "AccountWorkspaceEditTest.moc"
