#include <QtTest>

#include "ui/SplitterHandleStyle.h"

using namespace Mattermost;

class SplitterHandleStyleTest : public QObject
{
    Q_OBJECT

private slots:
    void horizontalSplitterKeepsWideHitAreaAndThinVisual()
    {
        const QRect handle(0, 0, 4, 120);
        const QRect divider = splitterDividerRect(handle, Qt::Horizontal);

        QCOMPARE(handle.width(), 4);
        QCOMPARE(divider.width(), SplitterVisibleDividerExtent);
        QCOMPARE(divider.height(), handle.height());
        QVERIFY(handle.contains(divider));
    }

    void verticalSplitterKeepsWideHitAreaAndThinVisual()
    {
        const QRect handle(0, 0, 160, 4);
        const QRect divider = splitterDividerRect(handle, Qt::Vertical);

        QCOMPARE(handle.height(), 4);
        QCOMPARE(divider.height(), SplitterVisibleDividerExtent);
        QCOMPARE(divider.width(), handle.width());
        QVERIFY(handle.contains(divider));
    }

    void emptyHandleHasNoDivider()
    {
        QVERIFY(splitterDividerRect(QRect(), Qt::Horizontal).isEmpty());
        QVERIFY(splitterDividerRect(QRect(), Qt::Vertical).isEmpty());
    }
};

QTEST_MAIN(SplitterHandleStyleTest)
#include "SplitterHandleStyleTest.moc"
