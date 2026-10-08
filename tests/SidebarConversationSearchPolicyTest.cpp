#include <QtTest>

#include "backend/SidebarConversationSearchPolicy.h"

using namespace Mattermost;

class SidebarConversationSearchPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void findsOldDmByPeerIdentity()
    {
        QVERIFY(matchesConversationSearch(QStringLiteral("yaroslav"),
                                          QStringLiteral("Old personal conversation"),
                                          QStringLiteral("Yaroslav Leschinskiy")));
        QVERIFY(matchesConversationSearch(QStringLiteral("j.olson"),
                                          QStringLiteral("Unlisted DM"), {},
                                          QStringLiteral("j.olson")));
        QVERIFY(matchesConversationSearch(QStringLiteral("@company"),
                                          QStringLiteral("Unlisted DM"), {}, {},
                                          QStringLiteral("joe@company.com")));
    }

    void findsOldGroupByDisplayName()
    {
        QVERIFY(matchesConversationSearch(QStringLiteral("archive"),
                                          QStringLiteral("Project Archive")));
        QVERIFY(!matchesConversationSearch(QStringLiteral("something-else"),
                                           QStringLiteral("Project Archive")));
    }

    void emptyFilterRestoresNormalRecentPolicy()
    {
        QVERIFY(!matchesConversationSearch(QString(), QStringLiteral("Any DM")));
        QVERIFY(!matchesConversationSearch(QStringLiteral("  "), QStringLiteral("Any GM")));
    }
};

QTEST_MAIN(SidebarConversationSearchPolicyTest)
#include "SidebarConversationSearchPolicyTest.moc"
