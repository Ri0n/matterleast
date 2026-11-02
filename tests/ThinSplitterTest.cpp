#include <QtTest>

#include <QCoreApplication>
#include <QImage>
#include <QPalette>
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

    void separatorStaysVisibleInDarkTheme()
    {
        ThinSplitter splitter(Qt::Horizontal);
        splitter.addWidget(new QWidget);
        splitter.addWidget(new QWidget);
        splitter.resize(420, 160);

        QPalette dark = splitter.palette();
        dark.setColor(QPalette::Window, QColor(24, 25, 26));
        dark.setColor(QPalette::WindowText, QColor(220, 222, 224));
        dark.setColor(QPalette::Mid, QColor(34, 35, 36));
        splitter.setPalette(dark);
        splitter.show();
        QCoreApplication::processEvents();

        QSplitterHandle* handle = splitter.handle(1);
        QVERIFY(handle);
        const QImage darkLine = handle->grab(handle->contentsRect()).toImage();
        QVERIFY(!darkLine.isNull());
        const QColor darkColor = darkLine.pixelColor(0, darkLine.height() / 2);
        QVERIFY2(darkColor.lightness() > dark.color(QPalette::Mid).lightness() + 25,
                 "Dark theme separator should be more visible than QPalette::Mid");
        QCOMPARE(handle->contentsRect().width(), ThinSplitter::VisibleHandleExtent);

        QPalette light = dark;
        light.setColor(QPalette::Window, QColor(245, 245, 245));
        light.setColor(QPalette::WindowText, QColor(35, 35, 35));
        light.setColor(QPalette::Mid, QColor(175, 175, 175));
        splitter.setPalette(light);
        QCoreApplication::processEvents();
        const QImage lightLine = handle->grab(handle->contentsRect()).toImage();
        QVERIFY(!lightLine.isNull());
        QCOMPARE(lightLine.pixelColor(0, lightLine.height() / 2),
                 light.color(QPalette::Mid));
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
