#include <QtTest>

#include <QAbstractScrollArea>
#include <QApplication>
#include <QLabel>
#include <QScrollBar>
#include <QToolButton>
#include <QWidget>

#include "ui/OverlayScrollBarManager.h"

namespace {

void settleEvents(int rounds = 6)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents();
    }
}

} // namespace

class InterceptedScrollArea : public QAbstractScrollArea
{
public:
    int intercepted = 0;
    bool handleBottomAction = true;

protected:
    bool event(QEvent* event) override
    {
        if (event->type() == Mattermost::OverlayScrollToEndEvent::eventType()
            && handleBottomAction) {
            ++intercepted;
            event->accept();
            return true;
        }
        return QAbstractScrollArea::event(event);
    }
};

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

    void customEndActionAndGenericBadgeDoNotChangeOrdinaryScrollAreas()
    {
        qWarning("BADGE_TEST: before area");
        InterceptedScrollArea area;
        area.resize(320, 240);
        area.verticalScrollBar()->setRange(0, 100);
        area.verticalScrollBar()->setValue(25);
        qWarning("BADGE_TEST: before show");
        area.show();
        qWarning("BADGE_TEST: after show");
        settleEvents();
        qWarning("BADGE_TEST: after settle");

        auto* button = area.findChild<QToolButton*>(
            QStringLiteral("mattermostOverlayScrollToEndButton"));
        auto* badge = area.findChild<QLabel*>(
            QStringLiteral("mattermostOverlayBottomBadge"));
        QVERIFY(button);
        QVERIFY(badge);
        QVERIFY(button->isVisible());
        QVERIFY(!badge->isVisible());

        qWarning("BADGE_TEST: before property");
        area.setProperty("mattermostOverlayBottomBadgeText",
                         QStringLiteral("128"));
        settleEvents();
        qWarning("BADGE_TEST: after property");
        QCOMPARE(badge->text(), QStringLiteral("128"));
        QVERIFY(badge->isVisible());
        QVERIFY(badge->geometry().bottom() <
                button->mapTo(badge->parentWidget(), QPoint(0, 0)).y());

        qWarning("BADGE_TEST: before first click");
        QTest::mouseClick(button, Qt::LeftButton);
        settleEvents();
        QCOMPARE(area.intercepted, 1);
        QCOMPARE(area.verticalScrollBar()->value(), 25);

        qWarning("BADGE_TEST: after first click");
        area.handleBottomAction = false;
        QTest::mouseClick(button, Qt::LeftButton);
        settleEvents();
        qWarning("BADGE_TEST: after second click");
        QCOMPARE(area.verticalScrollBar()->value(), 100);
        QVERIFY(!badge->isVisible());
        area.setProperty("mattermostOverlayBottomBadgeText", QString());
        QCOMPARE(badge->text(), QString());
    }

    void edgeButtonGuardsInterceptTheSurroundingArea()
    {
        QAbstractScrollArea area;
        area.resize(320, 240);
        area.verticalScrollBar()->setRange(0, 100);
        area.verticalScrollBar()->setValue(50);
        area.show();
        settleEvents();

        auto* button = area.findChild<QToolButton*>(
            QStringLiteral("mattermostOverlayScrollToEndButton"));
        auto* guard = area.findChild<QWidget*>(
            QStringLiteral("mattermostOverlayScrollToEndGuard"));
        QVERIFY(button);
        QVERIFY(guard);
        QVERIFY(button->isVisible());
        QVERIFY(guard->isVisible());
        QCOMPARE(button->parentWidget(), static_cast<QWidget*>(&area));
        QCOMPARE(guard->size(), button->size() * 3);
        QCOMPARE(button->pos(), guard->pos() + QPoint(button->width(), button->height()));
        QVERIFY(Mattermost::OverlayScrollBarManager::isEdgeNavigationExclusion(guard));
        QVERIFY(Mattermost::OverlayScrollBarManager::isEdgeNavigationExclusion(button));
        QVERIFY(!Mattermost::OverlayScrollBarManager::isEdgeNavigationExclusion(area.viewport()));

        // Check an empty part of the halo, to the left of the actual glyph.
        const QPoint haloPoint = guard->mapTo(&area, QPoint(2, guard->height() / 2));
        QCOMPARE(area.childAt(haloPoint), guard);
        QCOMPARE(area.childAt(button->geometry().center()), static_cast<QWidget*>(button));

        // The long overlay scrollbar must win over the halo at the right
        // edge; without this stacking order its thumb cannot be dragged.
        auto* overlay = area.findChild<QScrollBar*>(
            QStringLiteral("mattermostOverlayVerticalScrollBar"));
        QVERIFY(overlay);
        const QPoint thumbSidePoint(overlay->geometry().center().x(),
                                    guard->geometry().top() + 3);
        QCOMPARE(area.childAt(thumbSidePoint), static_cast<QWidget*>(overlay));

        area.verticalScrollBar()->setValue(area.verticalScrollBar()->maximum());
        settleEvents();
        QVERIFY(!guard->isVisible());
        QVERIFY(!button->isVisible());
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
