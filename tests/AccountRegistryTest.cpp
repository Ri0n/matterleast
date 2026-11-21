#include <QtTest>

#include "workspaces/AccountRegistry.h"

using namespace Mattermost;

class AccountRegistryTest final : public QObject
{
    Q_OBJECT
private slots:
    void storesTwoAccountsWithStableIds()
    {
        AccountRegistry registry;
        const QString a = registry.registerAuthenticated(
            QStringLiteral("https://chat-one.example.org/mm/"),
            QStringLiteral("user1"), QStringLiteral("alice"),
            QStringLiteral("chat-one — alice"));
        const QString b = registry.registerAuthenticated(
            QStringLiteral("https://chat-two.example.org/"),
            QStringLiteral("user2"), QStringLiteral("bob@example.org"),
            QStringLiteral("chat-two — bob"));
        QVERIFY(!a.isEmpty());
        QVERIFY(!b.isEmpty());
        QVERIFY(a != b);
        QCOMPARE(registry.layout().accounts.size(), 2);
        QCOMPARE(registry.layout().profiles.size(), 1);
        QCOMPARE(registry.layout().views.size(), 2);
        const QByteArray saved = registry.toJson();
        QVERIFY(!saved.isEmpty());
        QVERIFY(!saved.contains("password"));
        QVERIFY(!saved.contains("token"));

        AccountRegistry loaded;
        QVERIFY(loaded.load(saved));
        QCOMPARE(loaded.account(a)->loginName, QStringLiteral("alice"));
        QCOMPARE(loaded.account(b)->serverUserId, QStringLiteral("user2"));
        QCOMPARE(loaded.layout().activeViewId, registry.layout().activeViewId);
    }

    void reauthenticationDoesNotDuplicateAccountOrDiscardViews()
    {
        AccountRegistry registry;
        const QString accountId = registry.registerAuthenticated(
            QStringLiteral("https://chat.example.org/mm/"),
            QStringLiteral("user1"), QStringLiteral("alice"),
            QStringLiteral("Office"));
        const QString originalView = registry.layout().activeViewId;
        const QString again = registry.registerAuthenticated(
            QStringLiteral("https://CHAT.example.org:443/mm"),
            QStringLiteral("user1"), QStringLiteral("alice@example.org"),
            QStringLiteral("New display label"));
        QCOMPARE(again, accountId);
        QCOMPARE(registry.layout().accounts.size(), 1);
        QCOMPARE(registry.layout().views.size(), 1);
        QCOMPARE(registry.layout().activeViewId, originalView);
        QCOMPARE(registry.account(accountId)->name, QStringLiteral("Office"));
        QCOMPARE(registry.account(accountId)->loginName,
                 QStringLiteral("alice@example.org"));
    }

    void preservesLastGoodLayoutOnInvalidImport()
    {
        AccountRegistry registry;
        const QString id = registry.registerAuthenticated(
            QStringLiteral("https://chat.example.org/"),
            QStringLiteral("user1"), QStringLiteral("alice"), QStringLiteral("Home"));
        const QByteArray original = registry.toJson();
        QString error;
        QVERIFY(!registry.load(QByteArrayLiteral("{\"version\":999}"), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(registry.toJson(), original);
        QVERIFY(registry.account(id));
    }

    void rejectsMalformedIdentityWithoutChangingLayout()
    {
        AccountRegistry registry;
        QVERIFY(registry.registerAuthenticated(
            QStringLiteral("https://user:pass@chat.example.org"),
            QStringLiteral("user1"), QStringLiteral("alice"),
            QStringLiteral("Account")).isEmpty());
        QVERIFY(registry.registerAuthenticated(
            QStringLiteral("https://chat.example.org/?token=secret"),
            QStringLiteral("user1"), QStringLiteral("alice"),
            QStringLiteral("Account")).isEmpty());
        QVERIFY(registry.empty());
    }
};

QTEST_APPLESS_MAIN(AccountRegistryTest)
#include "AccountRegistryTest.moc"
