#include <QtTest>
#include <QAbstractItemModel>
#include <QLineEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeView>

#include "backend/AccountManager.h"
#include "login/LoginDialog.h"
#include "ui/ServersWorkspacePage.h"
#include "workspaces/WorkspaceTreeModel.h"

using namespace Mattermost;

class ServersWorkspacePageTest final : public QObject
{
    Q_OBJECT
private slots:
    void noServerStillHasTheMainWindowServersTab()
    {
        AccountManager accounts;
        QVERIFY(accounts.load({}));
        ServersWorkspacePage page(accounts);
        QCOMPARE(page.objectName(), QStringLiteral("serversWorkspacePage"));
        auto* tabs = page.findChild<QTabWidget*>(
            QStringLiteral("startupSidebarTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->tabText(0), QString());
        QVERIFY(!tabs->tabIcon(0).isNull());
        QCOMPARE(tabs->tabText(1), QStringLiteral("Channels"));
        QVERIFY(!tabs->isTabEnabled(1));
        QCOMPARE(tabs->currentIndex(), 0);

        auto* tree = page.findChild<QTreeView*>(
            QStringLiteral("serversWorkspaceTree"));
        QVERIFY(tree);
        QCOMPARE(tree->model(), accounts.workspaceTreeModel());
        QCOMPARE(tree->model()->rowCount(), 0);

        QSignalSpy add(&page, &ServersWorkspacePage::addServerRequested);
        auto* button = page.findChild<QPushButton*>(
            QStringLiteral("serversAdd"));
        QVERIFY(button);
        button->click();
        QCOMPARE(add.count(), 1);
    }

    void connectionProgressAppearsInsideServersTreeWithoutReset()
    {
        AccountManager accounts;
        QVERIFY(accounts.load({}));
        ServersWorkspacePage page(accounts);
        auto* tree = page.findChild<QTreeView*>(
            QStringLiteral("serversWorkspaceTree"));
        QVERIFY(tree);
        QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
        QSignalSpy inserts(tree->model(), &QAbstractItemModel::rowsInserted);
        QSignalSpy changes(tree->model(), &QAbstractItemModel::dataChanged);
        QSignalSpy removes(tree->model(), &QAbstractItemModel::rowsRemoved);
        auto* model = static_cast<WorkspaceTreeModel*>(accounts.workspaceTreeModel());
        model->setPendingServer(QStringLiteral("attempt-1"),
                                QStringLiteral("chat.example.org"),
                                QStringLiteral("Connecting…"));
        QCOMPARE(inserts.count(), 1);
        QCOMPARE(model->rowCount(), 1);
        const QModelIndex pending = model->index(0, 0);
        QCOMPARE(pending.data(WorkspaceTreeModel::NodeKindRole).toInt(),
                 int(WorkspaceTreeModel::PendingNode));
        QVERIFY(pending.data(Qt::DisplayRole).toString().contains(
                    QStringLiteral("Connecting…")));

        model->setPendingServer(QStringLiteral("attempt-1"),
                                QStringLiteral("chat.example.org"),
                                QStringLiteral("Sign-in failed"));
        QCOMPARE(inserts.count(), 1);
        QCOMPARE(changes.count(), 1);
        QCOMPARE(resets.count(), 0);
        QCOMPARE(pending.data(WorkspaceTreeModel::StatusRole).toString(),
                 QStringLiteral("Sign-in failed"));
        model->removePendingServer(QStringLiteral("attempt-1"));
        QCOMPARE(removes.count(), 1);
        QCOMPARE(model->rowCount(), 0);
        QCOMPARE(resets.count(), 0);
    }

    void addingServerSubmitsCredentialsWithoutOwningBackend()
    {
        // No Backend instance exists when opening the first-run dialog.
        auto* dialog = new LoginDialog(nullptr,
            QStringLiteral("https://chat.example.org"), QString());
        QSignalSpy accepted(dialog, &QDialog::accepted);
        auto* login = dialog->findChild<QLineEdit*>(
            QStringLiteral("username_lineEdit"));
        auto* password = dialog->findChild<QLineEdit*>(
            QStringLiteral("password_lineEdit"));
        QVERIFY(login);
        QVERIFY(password);
        login->setText(QStringLiteral("tester"));
        password->setText(QStringLiteral("nonsecret-test-password"));
        auto* button = dialog->findChild<QPushButton*>(
            QStringLiteral("login_pushButton"));
        QVERIFY(button);
        button->click();
        QCOMPARE(accepted.count(), 1);
        // QDialog deletes itself after accepted because it is a modal editor.
    }
};

QTEST_MAIN(ServersWorkspacePageTest)
#include "ServersWorkspacePageTest.moc"
