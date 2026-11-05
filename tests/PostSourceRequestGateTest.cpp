#include <QtTest>

#include "chat-area/PostSourceRequestGate.h"

class PostSourceRequestGateTest : public QObject
{
    Q_OBJECT

private slots:
    void attachesOnlyDemandFullyCoveredByInflightResponse()
    {
        Mattermost::PostSourceRequestGate gate;
        gate.begin(130, 149, 130, 149);

        QVERIFY(gate.isActive());
        QCOMPARE(gate.expectedRange().first, 130);
        QCOMPARE(gate.expectedRange().last, 149);

        QVERIFY(gate.canAttach(130, 149));
        QVERIFY(gate.canAttach(135, 145));
        QVERIFY(gate.canAttach(149, 149));
        QVERIFY(!gate.canAttach(120, 129));
        QVERIFY(!gate.canAttach(150, 160));
        QVERIFY(!gate.canAttach(125, 140));
        QVERIFY(!gate.canAttach(140, 155));
        QVERIFY(!gate.canAttach(120, 160));

        gate.attach(135, 145);
        QCOMPARE(gate.expectedRange().first, 130);
        QCOMPARE(gate.expectedRange().last, 149);
    }

    void hiddenMembershipShrinkDoesNotSwallowOlderPage()
    {
        Mattermost::PostSourceRequestGate gate;
        gate.begin(8, 18, 8, 18);

        // Exact production regression: the newest 11 posts are hidden
        // join/leave events. The filtered view shifts its older request to
        // [0,7] while [8,18] is in flight. The old adjacency rule joined
        // the requests and erroneously marked [0,7] complete without HTTP.
        QVERIFY(!gate.canAttach(0, 7));
        QVERIFY(!gate.canAttach(0, 8));
        QVERIFY(gate.canAttach(8, 18));
        QVERIFY(gate.canAttach(10, 16));
        QVERIFY(!gate.canAttach(17, 19));
    }

    void distantDemandCanBypassInflightRequest()
    {
        Mattermost::PostSourceRequestGate gate;
        gate.begin(130, 149, 130, 149);

        QVERIFY(!gate.canAttach(100, 119));
        QVERIFY(!gate.canAttach(161, 170));
        QVERIFY(!gate.canAttach(170, 160));
    }

    void finishReleasesWaitersAndResetsState()
    {
        Mattermost::PostSourceRequestGate gate;
        gate.begin(130, 149, 130, 149);
        gate.attach(135, 139);

        const QVector<Mattermost::PostSourceRequestGate::Range> waiters = gate.finish();
        QCOMPARE(waiters.size(), 2);
        QCOMPARE(waiters.at(0).first, 130);
        QCOMPARE(waiters.at(0).last, 149);
        QCOMPARE(waiters.at(1).first, 135);
        QCOMPARE(waiters.at(1).last, 139);

        QVERIFY(!gate.isActive());
        QVERIFY(!gate.expectedRange().isValid());
        QVERIFY(!gate.canAttach(130, 149));
        QVERIFY(gate.finish().isEmpty());
    }
};

QTEST_APPLESS_MAIN(PostSourceRequestGateTest)

#include "PostSourceRequestGateTest.moc"
