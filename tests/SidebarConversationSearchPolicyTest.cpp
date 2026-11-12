#include <QtTest>

#include <algorithm>
#include <cstdint>

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

    void filteredConversationsSortByLastPostWithStableTies()
    {
        struct Conversation {
            QString id;
            QString name;
            uint64_t lastPostAt;
        };
        QVector<Conversation> rows = {
            {QStringLiteral("z"), QStringLiteral("Zoe"), 0},
            {QStringLiteral("b"), QStringLiteral("Bob"), 200},
            {QStringLiteral("c"), QStringLiteral("Carla"), 300},
            {QStringLiteral("a"), QStringLiteral("alice"), 200},
            {QStringLiteral("y"), QStringLiteral("Anna"), 0},
            {QStringLiteral("aa"), QStringLiteral("Alice"), 200},
        };
        const auto sort = [&rows] {
            std::sort(rows.begin(), rows.end(),
                      [](const Conversation& left, const Conversation& right) {
                return conversationSearchOrderBefore(
                    left.lastPostAt, left.name, left.id,
                    right.lastPostAt, right.name, right.id);
            });
        };
        const auto ids = [&rows] {
            QStringList result;
            for (const Conversation& row : rows) {
                result.push_back(row.id);
            }
            return result;
        };
        const QStringList expected = {
            QStringLiteral("c"), QStringLiteral("a"), QStringLiteral("aa"),
            QStringLiteral("b"), QStringLiteral("y"), QStringLiteral("z"),
        };
        sort();
        QCOMPARE(ids(), expected);
        // Reconciliation may supply the same rows in a different order after
        // selecting/marking a DM. Stable search order must not depend on it.
        std::reverse(rows.begin(), rows.end());
        sort();
        QCOMPARE(ids(), expected);
    }

    void emptyFilterRestoresNormalRecentPolicy()
    {
        QVERIFY(!matchesConversationSearch(QString(), QStringLiteral("Any DM")));
        QVERIFY(!matchesConversationSearch(QStringLiteral("  "), QStringLiteral("Any GM")));
    }
};

QTEST_MAIN(SidebarConversationSearchPolicyTest)
#include "SidebarConversationSearchPolicyTest.moc"
