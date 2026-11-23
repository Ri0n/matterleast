#include <QtTest>

#include <QLineEdit>
#include <QPointer>
#include <QTreeView>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/AccountWorkspaceStack.h"
#include "workspaces/ViewConversationModel.h"
#include "workspaces/ViewSidebarModel.h"

using namespace Mattermost;

class AccountWorkspaceStackTest final : public QObject
{
    Q_OBJECT

private slots:
    void mergedSidebarIsViewScopedAndDoesNotReplaceAccountWidgets()
    {
        AccountWorkspaceStack stack;
        auto* first = new QWidget;
        auto* second = new QWidget;
        QWidget* servers = new QWidget;
        stack.setServersPage(servers);
        QVERIFY(stack.addWorkspace(QStringLiteral("a"), first));
        QVERIFY(stack.addWorkspace(QStringLiteral("b"), second));

        ViewConversationModel conversations;
        ViewSidebarModel sidebar;
        sidebar.setSources({
            {QStringLiteral("a"), QStringLiteral("Alpha"), nullptr},
            {QStringLiteral("b"), QStringLiteral("Beta"), nullptr}
        }, &conversations);
        auto* tree = stack.findChild<QTreeView*>(
            QStringLiteral("mergedViewSidebarTree"));
        auto* button = stack.findChild<QToolButton*>(
            QStringLiteral("mergedViewServersButton"));
        QVERIFY(tree);
        QVERIFY(button);
        stack.setMergedSidebarModel(&sidebar);
        QVERIFY(!tree->isHidden());
        QCOMPARE(tree->model(), static_cast<QAbstractItemModel*>(&sidebar));
        QCOMPARE(tree->model()->rowCount(), 3);
        QSignalSpy selected(&stack, &AccountWorkspaceStack::accountRequested);
        const QModelIndex root = sidebar.index(1, 0);
        QCOMPARE(root.data(ViewSidebarModel::AccountIdRole).toString(),
                 QStringLiteral("b"));
        stack.resize(650, 450);
        stack.show();
        QTest::qWaitForWindowExposed(&stack);
        QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                          tree->visualRect(root).center());
        QCOMPARE(selected.count(), 1);
        QCOMPARE(selected.at(0).at(0).toString(), QStringLiteral("b"));
        stack.showServers();
        QVERIFY(tree->isHidden());
        QVERIFY(stack.activateWorkspace(QStringLiteral("b")));
        QVERIFY(!tree->isHidden());
        QCOMPARE(stack.workspace(QStringLiteral("b")), second);
        QCOMPARE(stack.workspace(QStringLiteral("a")), first);
        stack.setMergedSidebarModel(nullptr);
        QVERIFY(tree->isHidden());
        QCOMPARE(stack.activeAccountId(), QStringLiteral("b"));
    }

    void switchPreservesIndependentLiveViews()
    {
        const QString accountOne = QStringLiteral("a");
        const QString accountTwo = QStringLiteral("b");
        AccountWorkspaceStack stack;

        auto* first = new QWidget;
        auto* firstEdit = new QLineEdit(first);
        firstEdit->setObjectName(QStringLiteral("firstComposer"));
        firstEdit->setText(QStringLiteral("unsent first draft"));
        auto* firstLayout = new QVBoxLayout(first);
        firstLayout->addWidget(firstEdit);

        auto* second = new QWidget;
        auto* secondEdit = new QLineEdit(second);
        secondEdit->setObjectName(QStringLiteral("secondComposer"));
        secondEdit->setText(QStringLiteral("unsent second draft"));
        auto* secondLayout = new QVBoxLayout(second);
        secondLayout->addWidget(secondEdit);

        QPointer<QWidget> firstGuard(first);
        QPointer<QWidget> secondGuard(second);
        QSignalSpy switched(&stack, &AccountWorkspaceStack::activeWorkspaceChanged);

        QVERIFY(stack.addWorkspace(accountOne, first));
        QVERIFY(stack.addWorkspace(accountTwo, second));
        QCOMPARE(stack.workspaceCount(), 2);
        QCOMPARE(stack.activeAccountId(), accountOne);
        QCOMPARE(stack.workspace(accountTwo), second);
        QVERIFY(!stack.addWorkspace(accountOne, new QWidget(&stack)));
        QVERIFY(!stack.addWorkspace(accountTwo, first));

        QVERIFY(stack.activateWorkspace(accountTwo));
        secondEdit->setText(QStringLiteral("second updated"));
        QVERIFY(stack.activateWorkspace(accountOne));
        firstEdit->setText(QStringLiteral("first updated"));
        QVERIFY(stack.activateWorkspace(accountTwo));
        QVERIFY(stack.activateWorkspace(accountOne));

        QVERIFY(firstGuard);
        QVERIFY(secondGuard);
        QCOMPARE(firstEdit->text(), QStringLiteral("first updated"));
        QCOMPARE(secondEdit->text(), QStringLiteral("second updated"));
        QCOMPARE(stack.workspaceCount(), 2);
        QCOMPARE(switched.count(), 5); // First registration + 4 switches.
    }

    void removalDoesNotDestroyOtherAccount()
    {
        const QString accountOne = QStringLiteral("a");
        const QString accountTwo = QStringLiteral("b");
        AccountWorkspaceStack stack;
        auto* first = new QWidget;
        auto* second = new QWidget;
        QPointer<QWidget> secondGuard(second);

        QVERIFY(stack.addWorkspace(accountOne, first));
        QVERIFY(stack.addWorkspace(accountTwo, second));
        QVERIFY(stack.activateWorkspace(accountTwo));
        QWidget* removed = stack.takeWorkspace(accountTwo);
        QCOMPARE(removed, second);
        QCOMPARE(removed->parentWidget(), nullptr);
        QCOMPARE(stack.activeAccountId(), accountOne);
        QCOMPARE(stack.workspaceCount(), 1);
        QVERIFY(!stack.activateWorkspace(accountTwo));
        QVERIFY(secondGuard);
        QCOMPARE(stack.workspace(accountOne), first);
        delete removed;

        QCOMPARE(stack.takeWorkspace(accountOne), first);
        delete first;
        QCOMPARE(stack.workspaceCount(), 0);
        QCOMPARE(stack.activeAccountId(), QString());
    }
};

QTEST_MAIN(AccountWorkspaceStackTest)
#include "AccountWorkspaceStackTest.moc"
