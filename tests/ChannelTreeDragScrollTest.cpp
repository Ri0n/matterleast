#include <QtTest>

#include <QApplication>
#include <QScrollBar>
#include <QWheelEvent>

#include "channel-tree/ChannelTree.h"

namespace Mattermost {

class ChannelTreeDragScrollTest : public QObject
{
    Q_OBJECT
private slots:
    void edgeScrollMovesTheRealSidebarViewportAndContentProbe()
    {
        ChannelTree tree;
        tree.resize(280, 180);
        tree.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        for (int i = 0; i < 90; ++i) {
            auto* item = new QTreeWidgetItem(&tree);
            item->setText(0, QString::number(i));
        }
        tree.show();
        QCoreApplication::processEvents();

        QScrollBar* scroll = tree.verticalScrollBar();
        QVERIFY(scroll);
        QVERIFY(scroll->maximum() > 50);
        tree.sidebarDragActive = true;
        const int startOffset = tree.verticalOffset();
        tree.scrollDuringDrag(50, QPoint(80, tree.viewport()->height() - 1));
        QVERIFY(scroll->value() > 0);
        QCOMPARE(tree.dragViewportScrollPixels,
                 tree.verticalOffset() - startOffset);

        tree.scrollDuringDrag(-12, QPoint(80, 0));
        QCOMPARE(tree.dragViewportScrollPixels,
                 tree.verticalOffset() - startOffset);
        tree.sidebarDragActive = false;
    }

    void wheelEventsAreInterceptedOnlyDuringDrag()
    {
        ChannelTree tree;
        tree.resize(280, 180);
        tree.setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        for (int i = 0; i < 70; ++i) {
            auto* item = new QTreeWidgetItem(&tree);
            item->setText(0, QString::number(i));
        }
        tree.show();
        QCoreApplication::processEvents();

        auto* scroll = tree.verticalScrollBar();
        QVERIFY(scroll);
        QVERIFY(scroll->maximum() > 30);
        const QPoint local(80, tree.viewport()->height() / 2);
        const QPoint global = tree.viewport()->mapToGlobal(local);
        QWheelEvent wheel(QPointF(local), QPointF(global), QPoint(),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::ScrollUpdate, false);
        tree.sidebarDragActive = true;
        qApp->installEventFilter(&tree);
        const int before = scroll->value();
        QCoreApplication::sendEvent(tree.viewport(), &wheel);
        qApp->removeEventFilter(&tree);
        QVERIFY(wheel.isAccepted());
        QVERIFY(scroll->value() > before);
        QVERIFY(tree.dragViewportScrollPixels > 0);
        tree.sidebarDragActive = false;
    }
};

} // namespace Mattermost

QTEST_MAIN(Mattermost::ChannelTreeDragScrollTest)
#include "ChannelTreeDragScrollTest.moc"
