#include <QtTest>

#include <QLineEdit>
#include <QPointer>
#include <QSplitter>
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

    void shellNeverIntroducesNestedSplitter()
    {
        AccountWorkspaceStack shell;
        QVERIFY(shell.findChildren<QSplitter*>().isEmpty());
        auto* account = new QWidget;
        auto* layout = new QVBoxLayout(account);
        auto* existing = new QSplitter(Qt::Horizontal, account);
        existing->addWidget(new QWidget);
        existing->addWidget(new QWidget);
        layout->addWidget(existing);
        QVERIFY(shell.addWorkspace(QStringLiteral("account"), account));
        QCOMPARE(shell.findChildren<QSplitter*>().size(), 1);
        QCOMPARE(shell.findChildren<QSplitter*>().front(), existing);
        QVERIFY(shell.activateWorkspace(QStringLiteral("account")));
        QCOMPARE(shell.findChildren<QSplitter*>().size(), 1);
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
