#include <QtTest>

#include "migrations/LegacyTokenRecovery.h"

using namespace Mattermost;

class LegacyTokenRecoveryTest final : public QObject
{
    Q_OBJECT
private slots:
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
