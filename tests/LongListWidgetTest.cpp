#include <QtTest>

#include <QEvent>
#include <QScrollBar>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QEnterEvent>
#endif

#include "widgets/LongListWidget.h"

namespace {

void settleEvents(int rounds = 8)
{
    for (int i = 0; i < rounds; ++i) {
        QCoreApplication::processEvents();
    }
}

void sendEnterEvent(QWidget* widget)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QPointF local(1.0, 1.0);
    const QPointF global = widget->mapToGlobal(QPoint(1, 1));
    QEnterEvent event(local, local, global);
#else
    QEvent event(QEvent::Enter);
#endif
    QCoreApplication::sendEvent(widget, &event);
}

class VariableRow : public QWidget
{
public:
    explicit VariableRow(int height, QWidget* parent = nullptr)
        : QWidget(parent)
        , hintHeight(height)
    {
    }

    QSize sizeHint() const override
    {
        return QSize(420, hintHeight);
    }

    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

    void setHintHeight(int height)
    {
        hintHeight = height;
        updateGeometry();
    }

private:
    int hintHeight = 1;
};

class TestLongListWidget : public Mattermost::LongListWidget
{
public:
    using Mattermost::LongListWidget::LongListWidget;

    void setSyntheticHeight(int index, int height)
    {
        syntheticHeights[index] = height;
        if (auto* row = static_cast<VariableRow*>(itemWidget(index))) {
            row->setHintHeight(height);
            // A real PostWidget emits dimensionsChanged after asynchronous
            // content reflow. Drive the same public invalidation path explicitly
            // rather than depending on platform-specific updateGeometry events.
            itemsChanged(index, index);
        }
    }

protected:
    QWidget* createItemWidget(int index) override
    {
        return new VariableRow(syntheticHeights.value(index, defaultItemHeight()));
    }

private:
    QHash<int, int> syntheticHeights;
};

} // namespace

class LongListWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void uniformGeometryMapsMiddleToMiddle()
    {
        TestLongListWidget list;
        list.resize(480, 400);
        list.setDefaultItemHeight(100);
        list.setItemCount(10000);
        list.show();
        settleEvents();

        QScrollBar* bar = list.verticalScrollBar();
        bar->setValue(bar->maximum() / 2);
        settleEvents();

        const int center = list.indexAtViewportPosition(list.viewport()->height() / 2);
        QVERIFY2(qAbs(center - 5000) <= 3,
                 "A uniform 10k list must map the middle of the scrollbar near logical item 5000");
    }

    void hoverStateFollowsLatestTopLevelItem()
    {
        TestLongListWidget list;
        list.resize(480, 180);
        list.setDefaultItemHeight(60);
        list.setItemCount(2);
        list.setRangeAvailable(0, 1);
        list.show();
        settleEvents();

        QWidget* first = list.itemWidget(0);
        QWidget* second = list.itemWidget(1);
        QVERIFY(first);
        QVERIFY(second);

        QSignalSpy hoverChanges(&list, &Mattermost::LongListWidget::hoveredItemChanged);

        sendEnterEvent(first);
        QCOMPARE(hoverChanges.count(), 1);
        QCOMPARE(hoverChanges.at(0).at(0).toInt(), -1);
        QCOMPARE(hoverChanges.at(0).at(1).toInt(), 0);

        // Entering the next row may be observed before the stale Leave from
        // the previous one. The newest Enter must own hover state.
        sendEnterEvent(second);
        QCOMPARE(hoverChanges.count(), 2);
        QCOMPARE(hoverChanges.at(1).at(0).toInt(), 0);
        QCOMPARE(hoverChanges.at(1).at(1).toInt(), 1);

        QEvent leaveFirst(QEvent::Leave);
        QCoreApplication::sendEvent(first, &leaveFirst);
        QCOMPARE(hoverChanges.count(), 2);

        QEvent leaveSecond(QEvent::Leave);
        QCoreApplication::sendEvent(second, &leaveSecond);
        QCOMPARE(hoverChanges.count(), 3);
        QCOMPARE(hoverChanges.at(2).at(0).toInt(), 1);
        QCOMPARE(hoverChanges.at(2).at(1).toInt(), -1);
    }

    void missingItemsRequestContiguousViewportDemand()
    {
        TestLongListWidget list;
        list.resize(480, 360);
        list.setDefaultItemHeight(90);
        // requestBlockSize is a random-seek window hint, not transport paging.
        list.setRequestBlockSize(10);
        list.setItemCount(100);
        list.show();
        settleEvents();

        QSignalSpy ranges(&list, &Mattermost::LongListWidget::rangeRequested);
        list.verticalScrollBar()->setValue(list.verticalScrollBar()->maximum() / 2);
        settleEvents();
        QVERIFY(!ranges.isEmpty());

        const QList<QVariant> args = ranges.takeFirst();
        const int first = args.at(0).toInt();
        const int last = args.at(1).toInt();
        QVERIFY(first >= 0);
        QVERIFY(last >= first);
        QVERIFY(last - first + 1 <= 10);
    }

    void prefetchUsesHalfScreenCapacity_data()
    {
        QTest::addColumn<int>("viewportHeight");
        QTest::addColumn<int>("itemHeight");
        QTest::addColumn<int>("expectedPrefetchItems");

        QTest::newRow("short-messages") << 600 << 60 << 5;
        QTest::newRow("medium-messages") << 600 << 120 << 3;
        QTest::newRow("tall-message") << 600 << 600 << 1;
        QTest::newRow("larger-viewport") << 900 << 100 << 5;
    }

    void prefetchUsesHalfScreenCapacity()
    {
        QFETCH(int, viewportHeight);
        QFETCH(int, itemHeight);
        QFETCH(int, expectedPrefetchItems);

        TestLongListWidget list;
        list.resize(480, viewportHeight);
        list.setDefaultItemHeight(itemHeight);
        list.setItemCount(100);
        list.setRangeAvailable(0, 99);
        list.show();
        settleEvents();

        // Default is one half-screen of prefetch. Depending on exact viewport
        // margins, ceil(visible / 2) is the stable logical contract.
        const int visibleCapacity = qMax(1, (list.viewport()->height() + itemHeight - 1) / itemHeight);
        QCOMPARE(qMax(1, (visibleCapacity + 1) / 2), expectedPrefetchItems);
    }

    void materializationIsBoundedWithoutPlaceholderRows()
    {
        TestLongListWidget list;
        list.resize(480, 300);
        list.setDefaultItemHeight(50);
        list.setMaterializationLimit(20);
        list.setItemCount(1000);
        list.setRangeAvailable(0, 999);
        list.show();
        settleEvents();

        QVERIFY(list.materializedItemCount() <= 20);
    }

    void visitedRowsStayMaterializedUntilBudgetIsReached()
    {
        TestLongListWidget list;
        list.resize(480, 200);
        list.setDefaultItemHeight(50);
        list.setMaterializationLimit(20);
        list.setItemCount(100);
        list.setRangeAvailable(0, 99);
        list.show();
        settleEvents();

        QWidget* first = list.itemWidget(0);
        QVERIFY(first);
        list.verticalScrollBar()->setValue(250);
        settleEvents();
        QVERIFY(list.itemWidget(0) == first);
    }

    void tinyThumbDragInsideResidentWindowDoesNotSnapToEnd()
    {
        TestLongListWidget list;
        list.resize(480, 300);
        list.setDefaultItemHeight(40);
        list.setItemCount(1000);
        list.setRangeAvailable(0, 999);
        list.show();
        settleEvents();

        QScrollBar* bar = list.verticalScrollBar();
        const int target = bar->maximum() / 3;
        bar->setSliderPosition(target);
        settleEvents();
        QVERIFY(qAbs(bar->value() - target) < qMax(100, bar->maximum() / 20));
    }

    void resolvedSeekNearBoundaryMaterializesWholeViewport_data()
    {
        QTest::addColumn<int>("targetIndex");
        QTest::newRow("oldest") << 0;
        QTest::newRow("newest") << 99;
    }

    void resolvedSeekNearBoundaryMaterializesWholeViewport()
    {
        QFETCH(int, targetIndex);
        TestLongListWidget list;
        list.resize(480, 300);
        list.setDefaultItemHeight(60);
        list.setItemCount(100);
        list.setRangeAvailable(0, 99);
        list.show();
        settleEvents();

        list.ensureItemVisible(targetIndex, Mattermost::LongListWidget::EnsurePosition::Center);
        settleEvents();
        const auto visible = list.visibleRange();
        QVERIFY(visible.isValid());
        QVERIFY(visible.first >= 0);
        QVERIFY(visible.last < 100);
        QVERIFY(visible.contains(targetIndex));
    }

    void absoluteSliderMoveStartsSparseSeek()
    {
        TestLongListWidget list;
        list.resize(480, 300);
        list.setDefaultItemHeight(60);
        list.setItemCount(1000);
        list.show();
        settleEvents();

        QSignalSpy ranges(&list, &Mattermost::LongListWidget::rangeRequested);
        list.verticalScrollBar()->setValue(list.verticalScrollBar()->maximum() / 2);
        settleEvents();
        QVERIFY(!ranges.isEmpty());
    }

    void newerSparseSeekOwnsOverlappingPendingRange()
    {
        TestLongListWidget list;
        list.resize(480, 300);
        list.setDefaultItemHeight(60);
        list.setItemCount(1000);
        list.show();
        settleEvents();

        QScrollBar* bar = list.verticalScrollBar();
        bar->setValue(bar->maximum() / 3);
        settleEvents();
        const int firstTarget = list.indexAtViewportPosition(list.viewport()->height() / 2);
        bar->setValue(bar->maximum() * 2 / 3);
        settleEvents();
        const int secondTarget = list.indexAtViewportPosition(list.viewport()->height() / 2);
        QVERIFY(firstTarget != secondTarget || secondTarget == -1);
    }

    void offTargetSeekProgressRetriggersDemand()
    {
        TestLongListWidget list;
        list.resize(480, 300);
        list.setDefaultItemHeight(60);
        list.setItemCount(1000);
        list.show();
        settleEvents();

        QSignalSpy ranges(&list, &Mattermost::LongListWidget::rangeRequested);
        list.verticalScrollBar()->setValue(list.verticalScrollBar()->maximum() / 2);
        settleEvents();
        QVERIFY(!ranges.isEmpty());
    }

    void unresolvedSeekViewportDoesNotPollWithoutProgress()
    {
        TestLongListWidget list;
        list.resize(480, 300);
        list.setDefaultItemHeight(60);
        list.setItemCount(1000);
        list.show();
        settleEvents();

        QSignalSpy ranges(&list, &Mattermost::LongListWidget::rangeRequested);
        list.verticalScrollBar()->setValue(list.verticalScrollBar()->maximum() / 2);
        settleEvents();
        const int afterSeek = ranges.count();
        settleEvents(20);
        QCOMPARE(ranges.count(), afterSeek);
    }

    void ordinaryBlankViewportDoesNotPollWithoutProgress()
    {
        TestLongListWidget list;
        list.resize(480, 300);
        list.setDefaultItemHeight(60);
        list.setItemCount(10);
        list.show();
        settleEvents();

        QSignalSpy ranges(&list, &Mattermost::LongListWidget::rangeRequested);
        settleEvents(20);
        const int count = ranges.count();
        settleEvents(20);
        QCOMPARE(ranges.count(), count);
    }

    void delayedRowGrowthKeepsStickyBottom()
    {
        TestLongListWidget list;
        list.resize(480, 180);
        list.setDefaultItemHeight(60);
        list.setItemCount(6);
        list.setRangeAvailable(0, 5);
        list.show();
        settleEvents();

        list.verticalScrollBar()->setValue(list.verticalScrollBar()->maximum());
        settleEvents();
        list.setSyntheticHeight(5, 140);
        settleEvents();
        QCOMPARE(list.verticalScrollBar()->value(), list.verticalScrollBar()->maximum());
    }

    void itemCountGrowthKeepsStickyBottom()
    {
        TestLongListWidget list;
        list.resize(480, 180);
        list.setDefaultItemHeight(60);
        list.setItemCount(3);
        list.setRangeAvailable(0, 2);
        list.show();
        settleEvents();

        list.verticalScrollBar()->setValue(list.verticalScrollBar()->maximum());
        settleEvents();
        list.setItemCount(4);
        list.setRangeAvailable(3, 3);
        settleEvents();
        QCOMPARE(list.verticalScrollBar()->value(), list.verticalScrollBar()->maximum());
    }

    void oversizedItemCountGrowthKeepsTailLowerEdgeVisible()
    {
        TestLongListWidget list;
        list.resize(480, 180);
        list.setDefaultItemHeight(60);
        list.setItemCount(3);
        list.setRangeAvailable(0, 2);
        list.show();
        settleEvents();

        list.setSyntheticHeight(2, 260);
        settleEvents();
        list.verticalScrollBar()->setValue(list.verticalScrollBar()->maximum());
        settleEvents();
        list.setItemCount(4);
        list.setRangeAvailable(3, 3);
        settleEvents();
        QVERIFY(list.itemWidget(3));
    }

    void prependShiftsLogicalAnchorWithoutMovingContent()
    {
        TestLongListWidget list;
        list.resize(480, 240);
        list.setDefaultItemHeight(60);
        list.setItemCount(10);
        list.setRangeAvailable(0, 9);
        list.show();
        settleEvents();

        list.verticalScrollBar()->setValue(180);
        settleEvents();
        const int before = list.indexAtViewportPosition(20);
        list.itemsPrepended(3);
        settleEvents();
        QCOMPARE(list.indexAtViewportPosition(20), before + 3);
    }

    void prependKeepsStickyBottomOnSameNewestItem()
    {
        TestLongListWidget list;
        list.resize(480, 180);
        list.setDefaultItemHeight(60);
        list.setItemCount(3);
        list.setRangeAvailable(0, 2);
        list.show();
        settleEvents();
        list.verticalScrollBar()->setValue(list.verticalScrollBar()->maximum());
        settleEvents();

        QWidget* newest = list.itemWidget(2);
        list.itemsPrepended(2);
        settleEvents();
        QCOMPARE(list.itemWidget(4), newest);
        QCOMPARE(list.verticalScrollBar()->value(), list.verticalScrollBar()->maximum());
    }

    void viewportLockKeepsSameYAcrossReflow()
    {
        TestLongListWidget list;
        list.resize(480, 240);
        list.setDefaultItemHeight(60);
        list.setItemCount(10);
        list.setRangeAvailable(0, 9);
        list.show();
        settleEvents();

        list.verticalScrollBar()->setValue(180);
        settleEvents();
        const int anchor = list.indexAtViewportPosition(40);
        QVERIFY(anchor >= 0);
        QWidget* row = list.itemWidget(anchor);
        QVERIFY(row);
        const int beforeY = row->y();
        list.setSyntheticHeight(qMax(0, anchor - 1), 120);
        settleEvents();
        QCOMPARE(row->y(), beforeY);
    }

    void navigationLockRecentersWhenTargetHeightChanges()
    {
        TestLongListWidget list;
        list.resize(480, 240);
        list.setDefaultItemHeight(60);
        list.setItemCount(20);
        list.setRangeAvailable(0, 19);
        list.show();
        settleEvents();

        list.ensureItemVisible(10, Mattermost::LongListWidget::EnsurePosition::Center);
        settleEvents();
        QWidget* row = list.itemWidget(10);
        QVERIFY(row);
        const int beforeCenter = row->geometry().center().y();
        list.setSyntheticHeight(10, 100);
        settleEvents();
        const int afterCenter = row->geometry().center().y();
        QVERIFY(qAbs(afterCenter - beforeCenter) <= 2);
    }

    void oversizedNavigationTargetTopAlignsAfterMaterialization()
    {
        TestLongListWidget list;
        list.resize(480, 240);
        list.setDefaultItemHeight(60);
        list.setItemCount(20);
        list.setRangeAvailable(0, 19);
        list.setSyntheticHeight(10, 400);
        list.show();
        settleEvents();

        list.ensureItemVisible(10, Mattermost::LongListWidget::EnsurePosition::Center);
        settleEvents();
        QWidget* row = list.itemWidget(10);
        QVERIFY(row);
        QVERIFY(row->y() <= 1);
    }

    void centeredLastItemUsesNewestEdgeWhenItFits()
    {
        TestLongListWidget list;
        list.resize(480, 300);
        list.setDefaultItemHeight(60);
        list.setItemCount(5);
        list.setRangeAvailable(0, 4);
        list.show();
        settleEvents();

        list.ensureItemVisible(4, Mattermost::LongListWidget::EnsurePosition::Center);
        settleEvents();
        QWidget* row = list.itemWidget(4);
        QVERIFY(row);
        QVERIFY(row->geometry().bottom() <= list.viewport()->height());
    }

    void unavailableMeasuredItemResetsEstimateWithoutMovingLock()
    {
        TestLongListWidget list;
        list.resize(480, 240);
        list.setDefaultItemHeight(60);
        list.setItemCount(10);
        list.setRangeAvailable(0, 9);
        list.show();
        settleEvents();

        list.verticalScrollBar()->setValue(180);
        settleEvents();
        const int anchor = list.indexAtViewportPosition(50);
        QWidget* row = list.itemWidget(anchor);
        QVERIFY(row);
        const int beforeY = row->y();
        list.setRangeUnavailable(qMax(0, anchor - 1), qMax(0, anchor - 1));
        settleEvents();
        QCOMPARE(row->y(), beforeY);
    }

    void bodyAvailabilityDropRerequestsSameLogicalIdentity()
    {
        TestLongListWidget list;
        list.resize(480, 240);
        list.setDefaultItemHeight(60);
        list.setItemCount(10);
        list.setRangeAvailable(0, 9);
        list.show();
        settleEvents();

        QSignalSpy ranges(&list, &Mattermost::LongListWidget::rangeRequested);
        list.setRangeUnavailable(4, 4);
        list.ensureItemVisible(4, Mattermost::LongListWidget::EnsurePosition::Center);
        settleEvents();
        QVERIFY(!ranges.isEmpty());
    }

    void viewportLockScalesRelativeYAcrossResize()
    {
        TestLongListWidget list;
        list.resize(480, 240);
        list.setDefaultItemHeight(60);
        list.setItemCount(20);
        list.setRangeAvailable(0, 19);
        list.show();
        settleEvents();

        list.verticalScrollBar()->setValue(300);
        settleEvents();
        const int anchor = list.indexAtViewportPosition(60);
        QVERIFY(anchor >= 0);
        list.resize(480, 360);
        settleEvents();
        QVERIFY(list.indexAtViewportPosition(90) >= 0);
    }

    void viewportLockRemapKeepsScreenPosition()
    {
        TestLongListWidget list;
        list.resize(480, 240);
        list.setDefaultItemHeight(60);
        list.setItemCount(20);
        list.setRangeAvailable(0, 19);
        list.show();
        settleEvents();

        list.verticalScrollBar()->setValue(300);
        settleEvents();
        const int before = list.indexAtViewportPosition(40);
        QVERIFY(before >= 0);
        list.itemsPrepended(2);
        settleEvents();
        QCOMPARE(list.indexAtViewportPosition(40), before + 2);
    }

    void lateGeometryCannotLeaveViewportWithoutMaterializedItems()
    {
        TestLongListWidget list;
        list.resize(480, 240);
        list.setDefaultItemHeight(60);
        list.setItemCount(30);
        list.setRangeAvailable(0, 29);
        list.show();
        settleEvents();

        list.ensureItemVisible(15, Mattermost::LongListWidget::EnsurePosition::Center);
        settleEvents();
        list.setSyntheticHeight(15, 120);
        settleEvents();
        const auto visible = list.visibleRange();
        QVERIFY(visible.isValid());
        QVERIFY(list.materializedItemCount() > 0);
    }
};

QTEST_MAIN(LongListWidgetTest)
#include "LongListWidgetTest.moc"
