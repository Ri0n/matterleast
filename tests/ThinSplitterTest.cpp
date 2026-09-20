#include <QtTest>

#include <QCoreApplication>
#include <QSplitter>
#include <QSplitterHandle>
#include <QWidget>

#include "ui/ThinSplitter.h"

using namespace Mattermost;

class ThinSplitterTest : public QObject
{
    Q_OBJECT

private slots:
    void horizontalHandleOverlapsOnlyFollowingPane()
    {
        ThinSplitter splitter(Qt::Horizontal);
        splitter.resize(420, 180);

        auto* left = new QWidget;
        auto* right = new QWidget;
        splitter.addWidget(left);
        splitter.addWidget(right);
        splitter.setSizes({200, 220});
        splitter.show();
        QCoreApplication::processEvents();

        QSplitterHandle* handle = splitter.handle(1);
        QVERIFY(handle);

        QCOMPARE(splitter.handleWidth(), ThinSplitter::VisibleHandleExtent);
        QCOMPARE(right->geometry().left() - left->geometry().right() - 1,
                 ThinSplitter::VisibleHandleExtent);
        QCOMPARE(handle->geometry().left(), left->geometry().right() + 1);
        QCOMPARE(handle->contentsRect().width(), ThinSplitter::VisibleHandleExtent);
        QCOMPARE(handle->geometry().right() - right->geometry().left() + 1,
                 ThinSplitter::TrailingHitPadding);
        QCOMPARE(handle->mask().boundingRect(), handle->contentsRect());
        QVERIFY(handle->testAttribute(Qt::WA_MouseNoMask));
    }

    void verticalHandleOverlapsOnlyFollowingPane()
    {
        ThinSplitter splitter(Qt::Vertical);
        splitter.resize(240, 420);

        auto* top = new QWidget;
        auto* bottom = new QWidget;
        splitter.addWidget(top);
        splitter.addWidget(bottom);
        splitter.setSizes({200, 220});
        splitter.show();
        QCoreApplication::processEvents();

        QSplitterHandle* handle = splitter.handle(1);
        QVERIFY(handle);

        QCOMPARE(bottom->geometry().top() - top->geometry().bottom() - 1,
                 ThinSplitter::VisibleHandleExtent);
        QCOMPARE(handle->geometry().top(), top->geometry().bottom() + 1);
        QCOMPARE(handle->contentsRect().height(), ThinSplitter::VisibleHandleExtent);
        QCOMPARE(handle->geometry().bottom() - bottom->geometry().top() + 1,
                 ThinSplitter::TrailingHitPadding);
        QCOMPARE(handle->mask().boundingRect(), handle->contentsRect());
        QVERIFY(handle->testAttribute(Qt::WA_MouseNoMask));
    }

    void legacyHandleWidthStateCannotWidenLayoutHandle()
    {
        QSplitter legacy(Qt::Horizontal);
        legacy.setHandleWidth(4);
        legacy.addWidget(new QWidget);
        legacy.addWidget(new QWidget);
        const QByteArray legacyState = legacy.saveState();

        ThinSplitter splitter(Qt::Horizontal);
        splitter.addWidget(new QWidget);
        splitter.addWidget(new QWidget);
        QVERIFY(splitter.restoreState(legacyState));

        // Production resets the public property after restore so subsequent
        // saves converge to 1 px. The custom handle itself is also fixed to
        // one layout pixel, so even a legacy state cannot make a visible gap.
        splitter.setHandleWidth(ThinSplitter::VisibleHandleExtent);
        QCOMPARE(splitter.handle(1)->sizeHint().width(),
                 ThinSplitter::VisibleHandleExtent);
    }
};

QTEST_MAIN(ThinSplitterTest)
#include "ThinSplitterTest.moc"
