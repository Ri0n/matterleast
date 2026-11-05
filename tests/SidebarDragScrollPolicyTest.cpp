#include <QtTest>

#include "channel-tree/SidebarDragScrollPolicy.h"

using Mattermost::SidebarDragScrollPolicy;

class SidebarDragScrollPolicyTest : public QObject
{
    Q_OBJECT
private slots:
    void noScrollingInViewportCenterOrOutsideSidebar()
    {
        const QSize view(280, 360);
        QCOMPARE(SidebarDragScrollPolicy::edgeStep(QPoint(100, 180), view, 1), 0);
        QCOMPARE(SidebarDragScrollPolicy::edgeStep(QPoint(-25, -20), view, 1), 0);
        QCOMPARE(SidebarDragScrollPolicy::edgeStep(QPoint(304, 380), view, 1), 0);
        QCOMPARE(SidebarDragScrollPolicy::edgeStep(QPoint(100, 180),
                                                  QSize(0, 360), 1), 0);
        QCOMPARE(SidebarDragScrollPolicy::edgeStep(QPoint(100, 180),
                                                  QSize(280, 0), 1), 0);
    }

    void scrollsRepeatedlyAtEdgesAndOutsideViewport()
    {
        const QSize view(280, 360);
        QVERIFY(SidebarDragScrollPolicy::edgeStep(QPoint(120, 12), view, 1) < 0);
        QVERIFY(SidebarDragScrollPolicy::edgeStep(QPoint(120, 352), view, 1) > 0);
        QCOMPARE(SidebarDragScrollPolicy::edgeStep(QPoint(120, -70), view, 1), -2);
        QCOMPARE(SidebarDragScrollPolicy::edgeStep(QPoint(120, 450), view, 1), 2);
        // Accelerate once outside the viewport, never exceeding two steps.
        const int near = SidebarDragScrollPolicy::edgeStep(QPoint(120, 350),
                                                            view, 2);
        const int far = SidebarDragScrollPolicy::edgeStep(QPoint(120, 420),
                                                           view, 2);
        QVERIFY(far > near);
        // A shorter viewport has distinct nonoverlapping top/bottom zones.
        QCOMPARE(SidebarDragScrollPolicy::edgeStep(QPoint(10, 10),
                                                   QSize(60, 24), 1), 0);
    }

    void wheelScrollRespectsPixelsAndNotches()
    {
        QCOMPARE(SidebarDragScrollPolicy::wheelStep(
                     QPoint(), QPoint(0, 120), 8, 3), -24);
        QCOMPARE(SidebarDragScrollPolicy::wheelStep(
                     QPoint(), QPoint(0, -120), 8, 3), 24);
        QCOMPARE(SidebarDragScrollPolicy::wheelStep(
                     QPoint(0, -17), QPoint(0, 120), 8, 3), 17);
        QCOMPARE(SidebarDragScrollPolicy::wheelStep(
                     QPoint(), QPoint(120, 0), 8, 3), 0);
    }

    void categoryDropProbeTracksScrollInContentPixels()
    {
        const int initial = SidebarDragScrollPolicy::categoryProbe(
            250, 120, 120, 0);
        QCOMPARE(initial, 250);
        // A stationary pointer reaches lower categories while scrolling down.
        QCOMPARE(SidebarDragScrollPolicy::categoryProbe(
            250, 120, 120, 90), 340);
        QCOMPARE(SidebarDragScrollPolicy::categoryProbe(
            250, 120, 120, -80), 170);
        QCOMPARE(SidebarDragScrollPolicy::categoryProbe(
            250, 150, 120, 90), 370);
    }

    void targetPositionIsClampedWhenCursorIsOutside()
    {
        const QSize view(240, 320);
        QCOMPARE(SidebarDragScrollPolicy::clampToViewport(
            QPoint(100, -300), view), QPoint(100, 0));
        QCOMPARE(SidebarDragScrollPolicy::clampToViewport(
            QPoint(100, 800), view), QPoint(100, 319));
        QCOMPARE(SidebarDragScrollPolicy::clampToViewport(
            QPoint(-8, 320), view), QPoint(0, 319));
    }
};

QTEST_APPLESS_MAIN(SidebarDragScrollPolicyTest)
#include "SidebarDragScrollPolicyTest.moc"
