#include <QtTest>

#include "chat-area/ChannelGapReconciliation.h"

using namespace Mattermost::ChannelGapReconciliation;

class ChannelGapReconciliationTest : public QObject
{
    Q_OBJECT

private slots:
    void afterOverlapProvesExactContext()
    {
        const auto match = matchAfter(
            9,
            QStringList { QStringLiteral("A"), QStringLiteral("F"), QStringLiteral("G") },
            QString(),
            QStringList { QStringLiteral("F"), QStringLiteral("G"), QStringLiteral("H") });

        QVERIFY(match.isExact());
        QCOMPARE(match.first, 11);
    }

    void afterInconsistentOverlapIsRejected()
    {
        const auto match = matchAfter(
            9,
            QStringList { QStringLiteral("F"), QStringLiteral("X"), QStringLiteral("G") },
            QString(),
            QStringList { QStringLiteral("F"), QStringLiteral("G"), QStringLiteral("H") });

        QVERIFY(match.conflict);
    }

    void afterContinuationEdgeProvesAdjacency()
    {
        const auto match = matchAfter(
            2,
            QStringList { QStringLiteral("D"), QStringLiteral("E") },
            QStringLiteral("F"),
            QStringList { QStringLiteral("F"), QStringLiteral("G"), QStringLiteral("H") });

        QVERIFY(match.isExact());
        QCOMPARE(match.first, 5);
    }

    void afterUnrelatedContinuationDoesNotInventAdjacency()
    {
        const auto match = matchAfter(
            2,
            QStringList { QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C") },
            QStringLiteral("D"),
            QStringList { QStringLiteral("F"), QStringLiteral("G"), QStringLiteral("H") });

        QVERIFY(!match.conflict);
        QVERIFY(!match.isExact());
    }

    void beforeOverlapProvesExactContext()
    {
        const auto match = matchBefore(
            20,
            QStringList { QStringLiteral("F"), QStringLiteral("G"), QStringLiteral("X") },
            QString(),
            QStringList { QStringLiteral("F"), QStringLiteral("G"), QStringLiteral("H") });

        QVERIFY(match.isExact());
        QCOMPARE(match.first, 20);
    }

    void beforeInconsistentOverlapIsRejected()
    {
        const auto match = matchBefore(
            20,
            QStringList { QStringLiteral("F"), QStringLiteral("X"), QStringLiteral("G") },
            QString(),
            QStringList { QStringLiteral("F"), QStringLiteral("G"), QStringLiteral("H") });

        QVERIFY(match.conflict);
    }

    void beforeContinuationEdgeProvesAdjacency()
    {
        const auto match = matchBefore(
            20,
            QStringList { QStringLiteral("D"), QStringLiteral("E") },
            QStringLiteral("H"),
            QStringList { QStringLiteral("F"), QStringLiteral("G"), QStringLiteral("H") });

        QVERIFY(match.isExact());
        QCOMPARE(match.first, 17);
    }

    void beforeUnrelatedContinuationDoesNotInventAdjacency()
    {
        const auto match = matchBefore(
            20,
            QStringList { QStringLiteral("D"), QStringLiteral("E") },
            QStringLiteral("C"),
            QStringList { QStringLiteral("F"), QStringLiteral("G"), QStringLiteral("H") });

        QVERIFY(!match.conflict);
        QVERIFY(!match.isExact());
    }

    void continuationKeepsUnresolvedGapAfter()
    {
        QCOMPARE(gapAfter(12, 13, QStringLiteral("D")), GapDecision::ReserveOne);
        QCOMPARE(gapAfter(12, 13, QString()), GapDecision::Reject);
        QCOMPARE(gapAfter(10, 13, QString()), GapDecision::None);
    }

    void continuationKeepsUnresolvedGapBefore()
    {
        QCOMPARE(gapBefore(20, 19, QStringLiteral("D")), GapDecision::ReserveOne);
        QCOMPARE(gapBefore(20, 19, QString()), GapDecision::Reject);
        QCOMPARE(gapBefore(22, 19, QString()), GapDecision::None);
    }

    void provisionalContextReservesUnknownSides()
    {
        auto guards = provisionalGuards(false, false);
        QCOMPARE(guards.left, 1);
        QCOMPARE(guards.right, 1);

        guards = provisionalGuards(true, false);
        QCOMPARE(guards.left, 0);
        QCOMPARE(guards.right, 1);

        guards = provisionalGuards(false, true);
        QCOMPARE(guards.left, 1);
        QCOMPARE(guards.right, 0);

        guards = provisionalGuards(true, true);
        QCOMPARE(guards.left, 0);
        QCOMPARE(guards.right, 0);
    }

    void absolutePageCannotEraseUnprovenGap()
    {
        QCOMPARE(absolutePageDecision(7, 9, 10, 12, false),
                 AbsolutePageDecision::Defer);
        QCOMPARE(absolutePageDecision(8, 10, 10, 12, false),
                 AbsolutePageDecision::Defer);
        QCOMPARE(absolutePageDecision(6, 8, 10, 12, false),
                 AbsolutePageDecision::Place);
        QCOMPARE(absolutePageDecision(7, 9, 10, 12, true),
                 AbsolutePageDecision::Reconcile);
    }

    void exampleDoesNotCollapseMissingDAndE()
    {
        const QStringList context {
            QStringLiteral("F"), QStringLiteral("G"), QStringLiteral("H")
        };

        const auto firstFetch = matchAfter(
            0,
            QStringList { QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C") },
            QStringLiteral("D"),
            context);
        QVERIFY(!firstFetch.isExact());
        QCOMPARE(gapAfter(3, 4, QStringLiteral("D")), GapDecision::ReserveOne);

        const auto secondFetch = matchAfter(
            3,
            QStringList { QStringLiteral("D"), QStringLiteral("E") },
            QStringLiteral("F"),
            context);
        QVERIFY(secondFetch.isExact());
        QCOMPARE(secondFetch.first, 6);
    }
};

QTEST_APPLESS_MAIN(ChannelGapReconciliationTest)

#include "ChannelGapReconciliationTest.moc"
