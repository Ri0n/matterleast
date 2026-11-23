#include <QtTest>
#include <type_traits>
#include <QAbstractItemModel>
#include <QFrame>
#include <QPushButton>
#include <QToolButton>
#include <QLayout>
#include <QTreeView>

#include "backend/AccountManager.h"
#include "navigation/ViewNavigationSession.h"
#include "ui/ServersSidebarPage.h"
#include "workspaces/WorkspaceTreeModel.h"
#include "workspaces/ViewConversationModel.h"

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
    void adoptionTakesOwnershipOfCatalogStrings()
    {
        // restoreAccount() forwards strings from _layout. adoptAuthenticated
        // replaces that layout, so const QString& parameters can become
        // dangling when the verified session is adopted (crash regression).
        using SafeAdoption = Account* (AccountManager::*)(
            Backend*, QString, QString, QString, bool);
        static_assert(std::is_same<decltype(&AccountManager::adoptAuthenticated),
                                   SafeAdoption>::value,
                      "adoptAuthenticated must own strings before replacing _layout");
    }
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

    void eachViewOwnsOneStableConversationModel()
    {
        AccountManager manager;
        QVERIFY(manager.load(topology().toJson()));
        auto* combined = manager.viewConversations(QStringLiteral("combined"));
        auto* single = manager.viewConversations(QStringLiteral("second"));
        QVERIFY(combined);
        QVERIFY(single);
        QVERIFY(combined != single);
        QCOMPARE(manager.activeViewConversations(),
                 static_cast<QAbstractItemModel*>(combined));
        QCOMPARE(combined->sorting(), QStringLiteral("recent"));
        QSignalSpy resets(combined, &QAbstractItemModel::modelReset);

        // Sorting belongs to the View, not to either account's backend.
        WorkspaceLayout edited = manager.layout();
        edited.views[0].directMessagesSorting = QStringLiteral("alpha");
        QString error;
        QVERIFY2(manager.applyLayoutEdits(edited, &error), qPrintable(error));
        QCOMPARE(manager.viewConversations(QStringLiteral("combined")), combined);
        QCOMPARE(combined->sorting(), QStringLiteral("alpha"));
        QCOMPARE(single->sorting(), QStringLiteral("recent"));
        QVERIFY(manager.activateView(QStringLiteral("second")));
        QCOMPARE(manager.activeViewConversations(),
                 static_cast<QAbstractItemModel*>(single));
        QCOMPARE(resets.count(), 0);
    }

    void removingServerFromViewDiscardsOnlyItsNavigation()
    {
        AccountManager manager;
        QVERIFY(manager.load(topology().toJson()));
        auto* session = manager.viewSession(QStringLiteral("combined"));
        QVERIFY(session);
        ViewNavigationSession::Location alpha;
        alpha.accountId = QStringLiteral("a");
        alpha.channelId = QStringLiteral("channel");
        ViewNavigationSession::Location beta = alpha;
        beta.accountId = QStringLiteral("b");
        QCOMPARE(session->openTab(alpha), 0);
        QCOMPARE(session->openTab(beta), 1);
        QSignalSpy reset(session, &QAbstractItemModel::modelReset);
        QSignalSpy removed(session, &QAbstractItemModel::rowsRemoved);

        auto edited = manager.layout();
        edited.views[0].accountIds.removeAll(QStringLiteral("a"));
        QString error;
        QVERIFY2(manager.applyLayoutEdits(edited, &error), qPrintable(error));
        QCOMPARE(reset.count(), 0);
        QCOMPARE(removed.count(), 1);
        QCOMPARE(session->rowCount(), 1);
        QCOMPARE(session->tabs().at(0)->accountId, QStringLiteral("b"));
        QCOMPARE(session->openTab(alpha), -1);
        QCOMPARE(manager.viewSession(QStringLiteral("second"))->rowCount(), 0);
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

    void inlineServerActionsHandleClicksWithoutOpeningServer()
    {
        AccountManager manager;
        QVERIFY(manager.load(topology().toJson()));
        ServersSidebarPage page(manager);
        page.resize(350, 450);
        page.show();
        QCoreApplication::processEvents();
        auto* tree = page.findChild<QTreeView*>(
            QStringLiteral("serversWorkspaceTree"));
        QVERIFY(tree);
        const QModelIndex server = tree->model()->index(1, 0);
        QVERIFY(server.isValid());
        QCOMPARE(server.data(WorkspaceTreeModel::NodeKindRole).toInt(),
                 int(WorkspaceTreeModel::AccountNode));
        const QRect row = tree->visualRect(server);
        QVERIFY(row.width() >= 60);
        QVERIFY(row.height() > 0);
        QSignalSpy settings(&page, &ServersSidebarPage::serverSettingsRequested);
        QSignalSpy connectRequested(&page, &ServersSidebarPage::retryRequested);
        QSignalSpy openRequested(&page, &ServersSidebarPage::accountOpenRequested);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(row.right() - 15, row.center().y()));
        QCOMPARE(settings.count(), 1);
        QCOMPARE(openRequested.count(), 0);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(row.right() - 45, row.center().y()));
        QCOMPARE(connectRequested.count(), 1);
        QCOMPARE(openRequested.count(), 0);
    }

    void serversPageUsesSharedTreeAndProvidesLayoutControls()
    {
        AccountManager manager;
        QVERIFY(manager.load(topology().toJson()));
        ServersSidebarPage page(manager);
        auto* tree = page.findChild<QTreeView*>(QStringLiteral("serversWorkspaceTree"));
        QVERIFY(tree);
        QCOMPARE(tree->model(), manager.workspaceTreeModel());
        QCOMPARE(tree->frameShape(), QFrame::NoFrame);
        QCOMPARE(tree->model()->rowCount(), 1);
        auto* layouts = page.findChild<QToolButton*>(QStringLiteral("serversEditLayout"));
        auto* add = page.findChild<QToolButton*>(QStringLiteral("serversAdd"));
        auto* toolbar = page.findChild<QWidget*>(QStringLiteral("serversToolbar"));
        QVERIFY(layouts);
        QVERIFY(add);
        QVERIFY(toolbar);
        QCOMPARE(toolbar->height(), 34);
        QCOMPARE(page.layout()->count(), 2); // Toolbar + tree; no footer controls.
        QVERIFY(add->text().isEmpty());
        QVERIFY(layouts->text().isEmpty());
        QVERIFY(!add->icon().isNull());
        QVERIFY(!layouts->icon().isNull());
        QVERIFY(!page.findChild<QPushButton*>(QStringLiteral("serversOpen")));
        QVERIFY(!page.findChild<QPushButton*>(QStringLiteral("serversSettings")));
        QVERIFY(!page.findChild<QPushButton*>(QStringLiteral("serversConnect")));
        QSignalSpy addRequested(&page, &ServersSidebarPage::addServerRequested);
        add->click();
        QCOMPARE(addRequested.count(), 1);
    }
};

QTEST_MAIN(AccountWorkspaceEditTest)
#include "AccountWorkspaceEditTest.moc"
