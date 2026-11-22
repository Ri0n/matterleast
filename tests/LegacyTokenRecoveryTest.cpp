#include <QtTest>

#include "migrations/LegacyTokenRecovery.h"

using namespace Mattermost;

class LegacyTokenRecoveryTest final : public QObject
{
    Q_OBJECT
private slots:
    void equivalentRootUrlsUseTheSameKeyAcrossQtVersions()
    {
        const QString username = QStringLiteral("s.ilinykh");
        const QString bare = SessionTokenStore::keyFor(
            QStringLiteral("https://chat.yadro.com"), username);
        const QString slash = SessionTokenStore::keyFor(
            QStringLiteral("https://chat.yadro.com/"), username);
        QCOMPARE(bare, slash);
        // Concrete regression from a pre-multi-account KWallet entry:
        // The old key had no slash; a post-refactor Backend appended '/'.
        QVERIFY2(bare.startsWith(QStringLiteral("login-token-v1-2cac322444b6")),
                 qPrintable(bare.left(27)));
    }

    void historicalServerUsernameCanBeRecovered()
    {
        WorkspaceLayout::Account record;
        record.id = QStringLiteral("account");
        record.serverBaseUrl = QStringLiteral("https://chat.example.org/");
        record.loginName = QStringLiteral("old.email@example.org");
        record.name = QStringLiteral("chat.example.org — s.ilinykh");
        QCOMPARE(LegacyTokenRecovery::candidateLoginName(record),
                 QStringLiteral("s.ilinykh"));
    }

    void fallbackUsesSameServerOriginAndKnownLegacyUsername()
    {
        WorkspaceLayout::Account record;
        record.serverBaseUrl = QStringLiteral("https://chat.example.org/mm/");
        record.loginName = QStringLiteral("email@example.org");
        record.name = QStringLiteral("chat.example.org — s.ilinykh");
        const auto locations = LegacyTokenRecovery::lookupCandidates(record);
        QCOMPARE(locations.size(), 4);
        QCOMPARE(locations[0].server, record.serverBaseUrl);
        QCOMPARE(locations[0].login, record.loginName);
        QCOMPARE(locations[1].server, record.serverBaseUrl);
        QCOMPARE(locations[1].login, QStringLiteral("s.ilinykh"));
        QCOMPARE(locations[2].server, QStringLiteral("https://chat.example.org"));
        QCOMPARE(locations[2].login, record.loginName);
        QCOMPARE(locations[3].server, QStringLiteral("https://chat.example.org"));
        QCOMPARE(locations[3].login, QStringLiteral("s.ilinykh"));
    }

    void sameLoginStillChecksHistoricalRootWhenCatalogUrlHasPath()
    {
        WorkspaceLayout::Account record;
        record.serverBaseUrl = QStringLiteral("https://chat.example.org/mm/");
        record.loginName = QStringLiteral("s.ilinykh");
        record.name = QStringLiteral("chat.example.org — s.ilinykh");
        const auto locations = LegacyTokenRecovery::lookupCandidates(record);
        QCOMPARE(locations.size(), 2);
        QCOMPARE(locations[0].server, record.serverBaseUrl);
        QCOMPARE(locations[1].server, QStringLiteral("https://chat.example.org"));
        QCOMPARE(locations[1].login, record.loginName);
    }

    void matchingOrUserEditedLabelIsNotInterpretedAsLegacyIdentity()
    {
        WorkspaceLayout::Account record;
        record.serverBaseUrl = QStringLiteral("https://chat.example.org");
        record.loginName = QStringLiteral("s.ilinykh");
        record.name = QStringLiteral("chat.example.org — s.ilinykh");
        QVERIFY(LegacyTokenRecovery::candidateLoginName(record).isEmpty());

        record.loginName = QStringLiteral("old.email@example.org");
        record.name = QStringLiteral("Work");
        QVERIFY(LegacyTokenRecovery::candidateLoginName(record).isEmpty());

        record.name = QStringLiteral("unrelated.example.org — s.ilinykh");
        QVERIFY(LegacyTokenRecovery::candidateLoginName(record).isEmpty());

        record.name = QStringLiteral("chat.example.org — ");
        QVERIFY(LegacyTokenRecovery::candidateLoginName(record).isEmpty());
    }

    void retainsInstallationPathAndRejectsInappropriateCandidate()
    {
        WorkspaceLayout::Account record;
        record.serverBaseUrl = QStringLiteral("https://chat.example.org/mm/");
        record.loginName = QStringLiteral("username@example.org");
        record.name = QStringLiteral("chat.example.org — s.ilinykh");
        QCOMPARE(LegacyTokenRecovery::candidateLoginName(record),
                 QStringLiteral("s.ilinykh"));

        record.name = QStringLiteral("chat.example.org — path/to/identity");
        QVERIFY(LegacyTokenRecovery::candidateLoginName(record).isEmpty());

        record.name = QStringLiteral("chat.example.org — username@example.org");
        QVERIFY(LegacyTokenRecovery::candidateLoginName(record).isEmpty());
    }
};

QTEST_APPLESS_MAIN(LegacyTokenRecoveryTest)
#include "LegacyTokenRecoveryTest.moc"
