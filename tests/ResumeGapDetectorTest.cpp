#include <QtTest>

#include "backend/ResumeGapDetector.h"

using namespace Mattermost;

class ResumeGapDetectorTest final : public QObject
{
    Q_OBJECT

private slots:
    void normalTickDoesNotRefresh()
    {
        ResumeGapDetector detector;
        QVERIFY(!detector.sample(100000));
        QVERIFY(!detector.sample(105000));
        QVERIFY(!detector.sample(110000));
    }

    void longGapTriggersExactlyOnce()
    {
        ResumeGapDetector detector;
        QVERIFY(!detector.sample(100000));
        QVERIFY(detector.sample(160000));
        QVERIFY(!detector.sample(165000));
        QVERIFY(detector.sample(225000));
    }

    void backwardsClockAdjustmentIsSafe()
    {
        ResumeGapDetector detector;
        QVERIFY(!detector.sample(150000));
        QVERIFY(!detector.sample(140000));
        QVERIFY(!detector.sample(145000));
    }
};

QTEST_MAIN(ResumeGapDetectorTest)
#include "ResumeGapDetectorTest.moc"
