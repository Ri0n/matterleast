#include <QtTest>

#include <QApplication>
#include <QMouseEvent>
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

        // Use the actual internal drag lifecycle rather than artificially
        // setting the active flag: QDrag::exec would swallow this wheel input.
        QTreeWidgetItem* source = tree.topLevelItem(2);
        QVERIFY(source);
        source->setData(0, ChannelTree::ItemKindRole,
                        int(ChannelTree::ChannelItemKind));
        tree.setCurrentItem(source);
        tree.startDrag(Qt::MoveAction);
        QVERIFY(tree.sidebarDragActive);
        QVERIFY(tree.dragScrollTimer);
        QVERIFY(tree.dragScrollTimer->isActive());

        auto* scroll = tree.verticalScrollBar();
        QVERIFY(scroll);
        QVERIFY(scroll->maximum() > 30);
        const QPoint local(80, tree.viewport()->height() / 2);
        const QPoint global = tree.viewport()->mapToGlobal(local);
        QWheelEvent wheel(QPointF(local), QPointF(global), QPoint(),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::ScrollUpdate, false);
        const int before = scroll->value();
        QCoreApplication::sendEvent(tree.viewport(), &wheel);
        QVERIFY(wheel.isAccepted());
        QVERIFY(scroll->value() > before);
        QVERIFY(tree.dragViewportScrollPixels > 0);

        // Crossing the top of the viewport does not hand off to native DnD
        // or turn the drag into an invalid target/cursor.
        tree.updateInternalDragPointer(QPoint(local.x(), -30));
        QVERIFY(tree.sidebarDragActive);
        const int afterWheel = scroll->value();
        tree.scrollDuringDrag(-2, QPoint(local.x(), -30));
        QVERIFY(scroll->value() < afterWheel);

        // A real mouse release outside the viewport cancels the internal
        // drag and must clear preview, timer and application event filter.
        const QPoint outside(-40, -40);
        QMouseEvent release(QEvent::MouseButtonRelease,
                            QPointF(outside), QPointF(outside),
                            QPointF(tree.viewport()->mapToGlobal(outside)),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(tree.viewport(), &release);
        QVERIFY(release.isAccepted());
        QVERIFY(!tree.sidebarDragActive);
        QVERIFY(!tree.dragScrollTimer);
        QVERIFY(!tree.dragPreview);
    }
};

} // namespace Mattermost

QTEST_MAIN(Mattermost::ChannelTreeDragScrollTest)
#include "ChannelTreeDragScrollTest.moc"
