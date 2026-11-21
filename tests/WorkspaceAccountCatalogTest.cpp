#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include "workspaces/WorkspaceLayout.h"

using namespace Mattermost;

class WorkspaceAccountCatalogTest final : public QObject
{
    Q_OBJECT
private slots:
    void storesTwoAccountsWithStableIds()
    {
        WorkspaceLayout registry;
        const QString a = registry.upsertAuthenticatedAccount(
            QStringLiteral("https://chat-one.example.org/mm/"),
            QStringLiteral("user1"), QStringLiteral("alice"),
            QStringLiteral("chat-one — alice"));
        const QString b = registry.upsertAuthenticatedAccount(
            QStringLiteral("https://chat-two.example.org/"),
            QStringLiteral("user2"), QStringLiteral("bob@example.org"),
            QStringLiteral("chat-two — bob"));
        QVERIFY(!a.isEmpty());
        QVERIFY(!b.isEmpty());
        QVERIFY(a != b);
        QCOMPARE(registry.accounts.size(), 2);
        QCOMPARE(registry.profiles.size(), 1);
        QCOMPARE(registry.views.size(), 2);
        const QByteArray saved = registry.toJson();
        QVERIFY(!saved.isEmpty());
        QVERIFY(!saved.contains("password"));
        QVERIFY(!saved.contains("token"));

        const auto loaded = WorkspaceLayout::fromJson(saved);
        QVERIFY(loaded.has_value());
        QCOMPARE(loaded->account(a)->loginName, QStringLiteral("alice"));
        QCOMPARE(loaded->account(b)->serverUserId, QStringLiteral("user2"));
        QCOMPARE(loaded->activeViewId, registry.activeViewId);
    }

    void reauthenticationDoesNotDuplicateAccountOrDiscardViews()
    {
        WorkspaceLayout registry;
        const QString accountId = registry.upsertAuthenticatedAccount(
            QStringLiteral("https://chat.example.org/mm/"),
            QStringLiteral("user1"), QStringLiteral("alice"),
            QStringLiteral("Office"));
        const QString originalView = registry.activeViewId;
        const QString again = registry.upsertAuthenticatedAccount(
            QStringLiteral("https://CHAT.example.org:443/mm"),
            QStringLiteral("user1"), QStringLiteral("alice@example.org"),
            QStringLiteral("New display label"));
        QCOMPARE(again, accountId);
        QCOMPARE(registry.accounts.size(), 1);
        QCOMPARE(registry.views.size(), 1);
        QCOMPARE(registry.activeViewId, originalView);
        QCOMPARE(registry.account(accountId)->name, QStringLiteral("Office"));
        QCOMPARE(registry.account(accountId)->loginName,
                 QStringLiteral("alice@example.org"));
    }

    void preservesLastGoodLayoutOnInvalidImport()
    {
        WorkspaceLayout registry;
        const QString id = registry.upsertAuthenticatedAccount(
            QStringLiteral("https://chat.example.org/"),
            QStringLiteral("user1"), QStringLiteral("alice"), QStringLiteral("Home"));
        const QByteArray original = registry.toJson();
        QString error;
        QVERIFY(!WorkspaceLayout::fromJson(
            QByteArrayLiteral("{\"version\":999}"), &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(registry.toJson(), original);
        QVERIFY(registry.account(id));
    }

    void resolvesLegacyAccountWithoutChangingItsNavigationIdentity()
    {
        WorkspaceLayout registry;
        const QString id = registry.upsertAuthenticatedAccount(
            QStringLiteral("https://old.example.org/"),
            QStringLiteral("olduser"), QStringLiteral("alice"),
            QStringLiteral("Office"));
        QByteArray original = registry.toJson();
        QJsonObject object = QJsonDocument::fromJson(original).object();
        QJsonArray records = object.value(QStringLiteral("accounts")).toArray();
        QJsonObject record = records[0].toObject();
        record.insert(QStringLiteral("serverBaseUrl"), QString());
        record.insert(QStringLiteral("serverUserId"), QString());
        record.insert(QStringLiteral("loginName"), QString());
        records.replace(0, record);
        object.insert(QStringLiteral("accounts"), records);
        const auto loaded = WorkspaceLayout::fromJson(QJsonDocument(object).toJson());
        QVERIFY(loaded.has_value());
        registry = *loaded;
        QVERIFY(!registry.account(id)->hasConnectionIdentity());
        const QString viewId = registry.activeViewId;
        QVERIFY(registry.resolveUnconfiguredAccount(
            id, QStringLiteral("https://new.example.org/mm/"),
            QStringLiteral("person"), QStringLiteral("alice@example.org")));
        QVERIFY(registry.account(id)->hasConnectionIdentity());
        QCOMPARE(registry.activeViewId, viewId);
        QCOMPARE(registry.account(id)->name, QStringLiteral("Office"));
        QCOMPARE(registry.account(id)->loginName, QStringLiteral("alice@example.org"));
    }

    void rejectsMalformedIdentityWithoutChangingLayout()
    {
        WorkspaceLayout registry;
        QVERIFY(registry.upsertAuthenticatedAccount(
            QStringLiteral("https://user:pass@chat.example.org"),
            QStringLiteral("user1"), QStringLiteral("alice"),
            QStringLiteral("Account")).isEmpty());
        QVERIFY(registry.upsertAuthenticatedAccount(
            QStringLiteral("https://chat.example.org/?token=secret"),
            QStringLiteral("user1"), QStringLiteral("alice"),
            QStringLiteral("Account")).isEmpty());
        QVERIFY(registry.accounts.isEmpty());
    }
};

QTEST_APPLESS_MAIN(WorkspaceAccountCatalogTest)
#include "WorkspaceAccountCatalogTest.moc"
