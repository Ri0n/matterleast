#include <QtTest>

#include <QLineEdit>
#include <QPointer>
#include <QVBoxLayout>

#include "ui/AccountWorkspaceStack.h"

using namespace Mattermost;

class AccountWorkspaceStackTest final : public QObject
{
    Q_OBJECT

private slots:
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

    void sharedSidebarFollowsWorkspaceWithoutDestroyingViewState()
    {
        AccountWorkspaceStack stack;
        QWidget* serversPage = new QWidget;
        stack.setServersPage(serversPage);
        auto* alpha = new QWidget;
        auto* beta = new QWidget;
        QVERIFY(stack.addWorkspace(QStringLiteral("a"), alpha));
        QVERIFY(stack.addWorkspace(QStringLiteral("b"), beta));
        QVERIFY(stack.activateWorkspace(QStringLiteral("a")));

        auto* shared = new QWidget;
        shared->setObjectName(QStringLiteral("retainedSharedSidebar"));
        QPointer<QWidget> guard(shared);
        auto* otherView = new QWidget;
        QPointer<QWidget> otherGuard(otherView);

        stack.setSharedSidebarWidget(shared);
        QCOMPARE(stack.sharedSidebarWidget(), shared);
        QVERIFY(stack.findChild<QWidget*>(
            QStringLiteral("mergedAccountSidebarPane")));
        stack.resize(850, 500);
        stack.show();
        QVERIFY(shared->isVisible());
        QVERIFY(stack.activateWorkspace(QStringLiteral("b")));
        QCOMPARE(stack.sharedSidebarWidget(), shared);
        QVERIFY(shared->isVisible());
        QCOMPARE(stack.workspace(QStringLiteral("a")), alpha);

        stack.setSharedSidebarWidget(otherView);
        QVERIFY(!shared->isVisible());
        QVERIFY(otherView->isVisible());
        stack.setSharedSidebarWidget(shared);
        QVERIFY(!otherView->isVisible());
        QVERIFY(shared->isVisible());

        stack.showServers();
        QVERIFY(!shared->isVisible());
        QVERIFY(stack.activateWorkspace(QStringLiteral("a")));
        QVERIFY(shared->isVisible());
        stack.setSharedSidebarWidget(nullptr);
        QVERIFY(!shared->isVisible());
        QVERIFY(guard);
        QVERIFY(otherGuard);
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
