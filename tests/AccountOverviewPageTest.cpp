#include <QtTest>

#include <QListWidget>
#include <QPushButton>

#include "ui/AccountOverviewPage.h"

using namespace Mattermost;

class AccountOverviewPageTest final : public QObject
{
    Q_OBJECT
private slots:
    void offlineAccountsRemainSelectable()
    {
        AccountOverviewPage page;
        const QVector<AccountOverviewPage::Entry> entries {
            {QStringLiteral("a"), QStringLiteral("Work"),
             QStringLiteral("work.example.org"), QStringLiteral("Connected")},
            {QStringLiteral("b"), QStringLiteral("Personal"),
             QStringLiteral("home.example.org"), QStringLiteral("Sign-in required")}
        };
        page.setEntries(entries, QStringLiteral("b"));
        auto* list = page.findChild<QListWidget*>(
            QStringLiteral("configuredAccountsList"));
        QVERIFY(list);
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->currentItem()->data(Qt::UserRole).toString(),
                 QStringLiteral("b"));
        QVERIFY(list->item(1)->text().contains(QStringLiteral("Sign-in required")));

        QSignalSpy retry(&page, &AccountOverviewPage::retryRequested);
        QSignalSpy signIn(&page, &AccountOverviewPage::signInRequested);
        for (QPushButton* button : page.findChildren<QPushButton*>()) {
            if (button->text() == QStringLiteral("Connect"))
                button->click();
            if (button->text() == QStringLiteral("Sign in…"))
                button->click();
        }
        QCOMPARE(retry.count(), 1);
        QCOMPARE(signIn.count(), 1);
        QCOMPARE(retry.first().first().toString(), QStringLiteral("b"));
        QCOMPARE(signIn.first().first().toString(), QStringLiteral("b"));
    }
};

QTEST_MAIN(AccountOverviewPageTest)
#include "AccountOverviewPageTest.moc"
