#include <QtTest>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QFrame>
#include <QScrollBar>
#include <QToolButton>

#include "ui/OverlayScrollBarManager.h"

namespace {

void settleEvents(int rounds = 6)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents();
    }
}

} // namespace

class OverlayScrollBarManagerTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        Mattermost::OverlayScrollBarManager::install(*qApp);
    }

    void edgeButtonsTrackPositionAndTriggerRealSliderActions()
    {
        QAbstractScrollArea area;
        area.resize(320, 240);
        area.verticalScrollBar()->setRange(0, 100);
        area.verticalScrollBar()->setPageStep(20);
        area.show();
        settleEvents();

        auto* toStart = area.findChild<QToolButton*>(
            QStringLiteral("mattermostOverlayScrollToStartButton"));
        auto* toEnd = area.findChild<QToolButton*>(
            QStringLiteral("mattermostOverlayScrollToEndButton"));
        QVERIFY(toStart);
        QVERIFY(toEnd);

        QVERIFY(!toStart->isVisible());
        QVERIFY(toEnd->isVisible());

        area.verticalScrollBar()->setValue(50);
        settleEvents();
        QVERIFY(toStart->isVisible());
        QVERIFY(toEnd->isVisible());

        QTest::mouseClick(toEnd, Qt::LeftButton);
        settleEvents();
        QCOMPARE(area.verticalScrollBar()->value(),
                 area.verticalScrollBar()->maximum());
        QVERIFY(toStart->isVisible());
        QVERIFY(!toEnd->isVisible());

        area.verticalScrollBar()->setValue(50);
        settleEvents();
        QTest::mouseClick(toStart, Qt::LeftButton);
        settleEvents();
        QCOMPARE(area.verticalScrollBar()->value(),
                 area.verticalScrollBar()->minimum());
    }

    void visibleButtonsHaveOneButtonSizeOfHoverExclusion()
    {
        using Mattermost::OverlayScrollBarManager;
        QAbstractScrollArea area;
        area.resize(320, 260);
        area.verticalScrollBar()->setRange(0, 100);
        area.verticalScrollBar()->setValue(50);
        area.show();
        settleEvents();

        auto* toStart = area.findChild<QToolButton*>(
            QStringLiteral("mattermostOverlayScrollToStartButton"));
        auto* toEnd = area.findChild<QToolButton*>(
            QStringLiteral("mattermostOverlayScrollToEndButton"));
        QVERIFY(toStart);
        QVERIFY(toEnd);
        QVERIFY(toStart->isVisible());
        QVERIFY(toEnd->isVisible());

        const QPoint top = toStart->mapToGlobal(toStart->rect().center());
        const QPoint bottom = toEnd->mapToGlobal(toEnd->rect().center());
        QVERIFY(OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(area, top));
        QVERIFY(OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(area, bottom));
        QVERIFY(OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(
            area, top - QPoint(toStart->width(), 0)));
        QVERIFY(OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(
            area, bottom - QPoint(toEnd->width(), 0)));
        QVERIFY(!OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(
            area, top - QPoint(2 * toStart->width() + 2, 0)));
        QVERIFY(!OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(
            area, bottom - QPoint(2 * toEnd->width() + 2, 0)));

        area.verticalScrollBar()->setValue(area.verticalScrollBar()->maximum());
        settleEvents();
        QVERIFY(!toEnd->isVisible());
        QVERIFY(!OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(area, bottom));
        QVERIFY(OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(area, top));

        area.verticalScrollBar()->setValue(area.verticalScrollBar()->minimum());
        settleEvents();
        QVERIFY(!toStart->isVisible());
        QVERIFY(!OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(area, top));
        QVERIFY(OverlayScrollBarManager::isNearVisibleEdgeNavigationButton(area, bottom));
    }

    void edgeButtonIsClickableAboveFloatingViewportToolbar()
    {
        QAbstractScrollArea area;
        area.resize(320, 260);
        area.verticalScrollBar()->setRange(0, 100);
        area.verticalScrollBar()->setValue(50);
        area.show();
        settleEvents();

        auto* end = area.findChild<QToolButton*>(
            QStringLiteral("mattermostOverlayScrollToEndButton"));
        QVERIFY(end);
        QVERIFY(end->isVisible());

        // The post toolbar belongs to the viewport, unlike the edge button.
        // Showing/re-raising the toolbar must not place a reaction control
        // above the edge button's physical click target.
        auto* toolbar = new QFrame(area.viewport());
        toolbar->setObjectName(QStringLiteral("postHoverActions"));
        toolbar->setGeometry(area.viewport()->rect());
        toolbar->show();
        toolbar->raise();
        settleEvents();

        const QPoint global = end->mapToGlobal(end->rect().center());
        QWidget* hit = QApplication::widgetAt(global);
        QCOMPARE(hit, static_cast<QWidget*>(end));
        QTest::mouseClick(hit, Qt::LeftButton);
        QCOMPARE(area.verticalScrollBar()->value(), area.verticalScrollBar()->maximum());
    }

    void edgeNavigationButtonsCanBeDisabledTogether()
    {
        QAbstractScrollArea area;
        area.resize(320, 240);
        area.verticalScrollBar()->setRange(0, 100);
        area.verticalScrollBar()->setValue(50);
        Mattermost::OverlayScrollBarManager::setEdgeNavigationButtonsEnabled(
            area, false);
        area.show();
        settleEvents();

        auto* toStart = area.findChild<QToolButton*>(
            QStringLiteral("mattermostOverlayScrollToStartButton"));
        auto* toEnd = area.findChild<QToolButton*>(
            QStringLiteral("mattermostOverlayScrollToEndButton"));
        QVERIFY(toStart);
        QVERIFY(toEnd);
        QVERIFY(!toStart->isVisible());
        QVERIFY(!toEnd->isVisible());

        auto* overlay = area.findChild<QScrollBar*>(
            QStringLiteral("mattermostOverlayVerticalScrollBar"));
        QVERIFY(overlay);
        QVERIFY(overlay->isVisible());

        Mattermost::OverlayScrollBarManager::setEdgeNavigationButtonsEnabled(
            area, true);
        settleEvents();
        QVERIFY(toStart->isVisible());
        QVERIFY(toEnd->isVisible());
    }

    void startActionCanBeDisabledWithoutAffectingEndAction()
    {
        QAbstractScrollArea area;
        area.resize(320, 240);
        area.verticalScrollBar()->setRange(0, 100);
        area.verticalScrollBar()->setValue(50);
        Mattermost::OverlayScrollBarManager::setScrollToStartButtonEnabled(area, false);
        area.show();
        settleEvents();

        auto* toStart = area.findChild<QToolButton*>(
            QStringLiteral("mattermostOverlayScrollToStartButton"));
        auto* toEnd = area.findChild<QToolButton*>(
            QStringLiteral("mattermostOverlayScrollToEndButton"));
        QVERIFY(toStart);
        QVERIFY(toEnd);
        QVERIFY(!toStart->isVisible());
        QVERIFY(toEnd->isVisible());

        Mattermost::OverlayScrollBarManager::setScrollToStartButtonEnabled(area, true);
        settleEvents();
        QVERIFY(toStart->isVisible());
    }
};

QTEST_MAIN(OverlayScrollBarManagerTest)
#include "OverlayScrollBarManagerTest.moc"
