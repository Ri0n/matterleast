#include <QtTest>

#include <QListView>

#include "workspaces/AccountListModel.h"
#include <QPushButton>

#include "ui/AccountOverviewPage.h"

using namespace Mattermost;

class AccountOverviewPageTest final : public QObject
{
    Q_OBJECT
private slots:
    void offlineAccountsRemainSelectable()
    {
        AccountListModel model;
        AccountOverviewPage page(model);
        const QVector<AccountListModel::Entry> entries {
            {QStringLiteral("a"), QStringLiteral("Work"),
             QStringLiteral("work.example.org"), AccountConnectionState::Connected},
            {QStringLiteral("b"), QStringLiteral("Personal"),
             QStringLiteral("home.example.org"), AccountConnectionState::AuthenticationRequired}
        };
        model.setEntries(entries, QStringLiteral("b"));
        auto* list = page.findChild<QListView*>(
            QStringLiteral("configuredAccountsList"));
        QVERIFY(list);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(model.idAt(list->currentIndex().row()), QStringLiteral("b"));
        QVERIFY(model.data(model.index(1, 0), AccountListModel::StatusRole)
                    .toString().contains(QStringLiteral("Sign-in required")));

        QSignalSpy retry(&page, &AccountOverviewPage::retryRequested);
        QSignalSpy signIn(&page, &AccountOverviewPage::signInRequested);
        QSignalSpy manage(&page, &AccountOverviewPage::manageServersRequested);
        for (QPushButton* button : page.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Connect"))
                button->click();
            if (button->text() == QStringLiteral("Sign in…"))
                button->click();
        }
        auto* manageButton = page.findChild<QPushButton*>(
            QStringLiteral("overviewManageServers"));
        QVERIFY(manageButton);
        manageButton->click();
        QCOMPARE(manage.count(), 1);
        QCOMPARE(retry.count(), 1);
        QCOMPARE(signIn.count(), 1);
        QCOMPARE(retry.first().first().toString(), QStringLiteral("b"));
        QCOMPARE(signIn.first().first().toString(), QStringLiteral("b"));
    }
};

QTEST_MAIN(AccountOverviewPageTest)
#include "AccountOverviewPageTest.moc"
