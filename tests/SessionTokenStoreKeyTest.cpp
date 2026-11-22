#include <QtTest>
#include "backend/SessionTokenStore.h"

using namespace Mattermost;

class SessionTokenStoreKeyTest final : public QObject
{
    Q_OBJECT
private slots:
    void rootSlashDoesNotChangeStoredKey()
    {
        const QString user = QStringLiteral("s.ilinykh");
        const QString plain = SessionTokenStore::keyFor(
            QStringLiteral("https://chat.yadro.com"), user);
        const QString slash = SessionTokenStore::keyFor(
            QStringLiteral("https://chat.yadro.com/"), user);
        QCOMPARE(slash, plain);
        QVERIFY(plain.startsWith(QStringLiteral("login-token-v1-2cac322444b6")));
    }

    void differentAccountOrInstallationPathRemainsDistinct()
    {
        const auto first = SessionTokenStore::keyFor(
            QStringLiteral("https://chat.example.org/"), QStringLiteral("alice"));
        const auto second = SessionTokenStore::keyFor(
            QStringLiteral("https://chat.example.org/"), QStringLiteral("bob"));
        const auto installed = SessionTokenStore::keyFor(
            QStringLiteral("https://chat.example.org/mm/"), QStringLiteral("alice"));
        QVERIFY(first != second);
        QVERIFY(first != installed);
    }
};

QTEST_APPLESS_MAIN(SessionTokenStoreKeyTest)
#include "SessionTokenStoreKeyTest.moc"
