#include <QtTest>

#include "chat-area/ThreadNavigationPlacement.h"
#include "chat-area/ThreadTimelineSizing.h"

using Mattermost::ThreadNavigationPlacement;
using Mattermost::ThreadSeekAnchor;
using Mattermost::threadSeekEstimate;

class ThreadNavigationPlacementTest : public QObject
{
    Q_OBJECT

private slots:
    void estimatedTargetBlocksMaterialization()
    {
        ThreadNavigationPlacement placement;
        placement.trackEstimated(QStringLiteral("target"), 12);

        QVERIFY(placement.isActive());
        QCOMPARE(placement.postId(), QStringLiteral("target"));
        QCOMPARE(placement.index(), 12);
        QVERIFY(placement.blocksMaterialization(QStringLiteral("target")));
        QVERIFY(!placement.blocksMaterialization(QStringLiteral("other")));
    }

    void repeatedLookupKeepsEstimatedTargetBlocked()
    {
        ThreadNavigationPlacement placement;
        placement.trackEstimated(QStringLiteral("target"), 12);
        placement.trackExistingProvisional(QStringLiteral("target"), 12);

        QVERIFY(placement.isActive());
        QCOMPARE(placement.index(), 12);
        QVERIFY(placement.blocksMaterialization(QStringLiteral("target")));
    }

    void differentExactIndexReleasesEstimatedTarget()
    {
        ThreadNavigationPlacement placement;
        placement.trackEstimated(QStringLiteral("target"), 12);

        const auto confirmation = placement.confirmExactWindow(
            20,
            QStringList { QStringLiteral("before"),
                          QStringLiteral("target"),
                          QStringLiteral("after") });

        QVERIFY(confirmation.isValid());
        QVERIFY(confirmation.wasEstimated);
        QVERIFY(confirmation.moved());
        QCOMPARE(confirmation.previousIndex, 12);
        QCOMPARE(confirmation.authoritativeIndex, 21);
        QVERIFY(!placement.isActive());
        QVERIFY(!placement.blocksMaterialization(QStringLiteral("target")));
    }

    void sameExactIndexStillReleasesEstimatedTarget()
    {
        ThreadNavigationPlacement placement;
        placement.trackEstimated(QStringLiteral("target"), 21);

        const auto confirmation = placement.confirmExactWindow(
            20,
            QStringList { QStringLiteral("before"),
                          QStringLiteral("target"),
                          QStringLiteral("after") });

        QVERIFY(confirmation.isValid());
        QVERIFY(confirmation.wasEstimated);
        QVERIFY(!confirmation.moved());
        QCOMPARE(confirmation.previousIndex, 21);
        QCOMPARE(confirmation.authoritativeIndex, 21);
        QVERIFY(!placement.isActive());
        QVERIFY(!placement.blocksMaterialization(QStringLiteral("target")));
    }

    void unrelatedExactWindowKeepsEstimatedTargetBlocked()
    {
        ThreadNavigationPlacement placement;
        placement.trackEstimated(QStringLiteral("target"), 12);

        const auto confirmation = placement.confirmExactWindow(
            20,
            QStringList { QStringLiteral("first"), QStringLiteral("second") });

        QVERIFY(!confirmation.isValid());
        QVERIFY(placement.isActive());
        QCOMPARE(placement.index(), 12);
        QVERIFY(placement.blocksMaterialization(QStringLiteral("target")));
    }

    void cachedProvisionalTargetRemainsMaterializable()
    {
        ThreadNavigationPlacement placement;
        placement.trackExistingProvisional(QStringLiteral("target"), 7);

        QVERIFY(placement.isActive());
        QVERIFY(!placement.blocksMaterialization(QStringLiteral("target")));

        const auto confirmation = placement.confirmExactWindow(
            6,
            QStringList { QStringLiteral("before"), QStringLiteral("target") });

        QVERIFY(confirmation.isValid());
        QVERIFY(!confirmation.wasEstimated);
        QCOMPARE(confirmation.previousIndex, 7);
        QCOMPARE(confirmation.authoritativeIndex, 7);
        QVERIFY(!placement.isActive());
    }


    void seekEstimateUsesNearestKnownAnchors()
    {
        const std::vector<ThreadSeekAnchor> anchors {
            { 40, 4'000 },
            { 140, 14'000 },
        };

        const auto estimate = threadSeekEstimate(50, 175, 1'000, 100'000, anchors);

        QCOMPARE(estimate.lowerIndex, 40);
        QCOMPARE(estimate.upperIndex, 140);
        QCOMPARE(estimate.createAt, quint64(5'000));
    }

    void seekEstimateFallsBackToTimelineEdges()
    {
        const auto estimate = threadSeekEstimate(50, 100, 1'000, 11'000, {});

        QCOMPARE(estimate.lowerIndex, 0);
        QCOMPARE(estimate.upperIndex, 100);
        QCOMPARE(estimate.createAt, quint64(6'000));
    }
};

QTEST_APPLESS_MAIN(ThreadNavigationPlacementTest)

#include "ThreadNavigationPlacementTest.moc"
