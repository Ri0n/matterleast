#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QtTest>

#include "chat-area/FormattingToolbarOverlay.h"
#include "ui/FlowLayout.h"

using namespace Mattermost;

class FormattingToolbarOverlayTest : public QObject
{
    Q_OBJECT

private slots:
    void previewExpandsUpwardAndTracksGrowingEditor()
    {
        QWidget host;
        host.resize(480, 440);
        auto* layout = new QVBoxLayout(&host);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* log = new QWidget(&host);
        log->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        layout->addWidget(log, 1);
        auto* footer = new QWidget(&host);
        auto* footerLayout = new QVBoxLayout(footer);
        footerLayout->setContentsMargins(12, 0, 12, 0);
        auto* toolbar = new QWidget(footer);
        toolbar->setObjectName(QStringLiteral("formattingToolbar"));
        auto* flow = new FlowLayout(toolbar, 0, 1);
        for (int i = 0; i < 8; ++i) {
            auto* button = new QPushButton(QString::number(i), toolbar);
            button->setFixedSize(35, 26);
            flow->addWidget(button);
        }
        footerLayout->addWidget(toolbar);
        auto* editor = new QWidget(footer);
        editor->setFixedHeight(36);
        footerLayout->addWidget(editor);
        layout->addWidget(footer);

        FormattingToolbarOverlay overlay(host, *editor, *toolbar,
                                         *footerLayout);
        auto* preview = new QWidget(overlay.panel());
        auto* eye = new QPushButton(overlay.panel());
        eye->setFixedSize(30, 30);
        overlay.installPreview(*preview, *eye);
        host.show();
        QApplication::processEvents();
        const int logHeight = log->height();
        overlay.setExpanded(true);
        QEvent enterEditor(QEvent::Enter);
        QApplication::sendEvent(editor, &enterEditor);
        QTest::qWait(150);
        QVERIFY(overlay.panel()->isVisible());
        const int collapsedHeight = overlay.panel()->height();

        overlay.setPreviewVisible(true);
        QApplication::processEvents();
        QVERIFY(preview->isVisible());
        QVERIFY(overlay.panel()->height() > collapsedHeight);
        QCOMPARE(log->height(), logHeight);
        QTRY_COMPARE(editor->mapTo(&host, QPoint()).y()
                         - overlay.panel()->geometry().bottom(), 4);

        const int originalEditorTop = editor->mapTo(&host, QPoint()).y();
        const int growth = 124 - 36;
        editor->setFixedHeight(124);
        // Offscreen Qt does not reliably propagate QWidget size hints to its
        // parent's layout. Explicitly grow the footer as the real composer
        // footer does, to test that the overlay tracks an actual upward move.
        footer->setFixedHeight(footer->height() + growth);
        layout->activate();
        QApplication::processEvents();
        QTRY_VERIFY(editor->mapTo(&host, QPoint()).y() < originalEditorTop);
        QTRY_COMPARE(editor->mapTo(&host, QPoint()).y()
                         - overlay.panel()->geometry().bottom(), 4);
        QCOMPARE(log->height(), logHeight - growth);
        overlay.setPreviewVisible(false);
        QApplication::processEvents();
        QVERIFY(!preview->isVisible());
        QCOMPARE(overlay.panel()->height(), collapsedHeight);
    }

    void focusTransitionsDoNotResizeChatViewport()
    {
        QWidget host;
        host.resize(520, 400);
        auto* mainLayout = new QVBoxLayout(&host);
        mainLayout->setContentsMargins(0, 0, 0, 0);
        mainLayout->setSpacing(2);

        QWidget log(&host);
        log.setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        mainLayout->addWidget(&log, 1);

        QWidget footer(&host);
        auto* footerLayout = new QVBoxLayout(&footer);
        // Simulate the editor being inset between attachment/send buttons.
        // The floating toolbar must still reach both chat edges.
        footerLayout->setContentsMargins(22, 0, 15, 0);
        footerLayout->setSpacing(0);
        mainLayout->addWidget(&footer);

        QWidget toolbar(&footer);
        toolbar.setObjectName(QStringLiteral("formattingToolbar"));
        auto* flow = new FlowLayout(&toolbar, 0, 1);
        for (int i = 0; i < 11; ++i) {
            auto* button = new QPushButton(QString::number(i), &toolbar);
            button->setFixedSize(35, 26);
            flow->addWidget(button);
        }
        footerLayout->addWidget(&toolbar);

        QWidget editor(&footer);
        editor.setFixedHeight(38);
        footerLayout->addWidget(&editor);

        FormattingToolbarOverlay overlay(host, editor, toolbar,
                                         *footerLayout);

        host.show();
        QApplication::processEvents();
        // Removing the toolbar from the footer must leave only the editor.
        QCOMPARE(footerLayout->indexOf(&toolbar), -1);
        QCOMPARE(toolbar.parentWidget(), overlay.panel());
        QVERIFY(editor.width() < host.width());
        QVERIFY(editor.mapTo(&host, QPoint()).x() > 0);
        const int viewportHeight = log.height();
        const int editorY = editor.mapTo(&host, QPoint()).y();

        overlay.setExpanded(true);
        QTest::qWait(150);
        QVERIFY(!overlay.panel()->isVisible());
        QEvent enterEditor(QEvent::Enter);
        QApplication::sendEvent(&editor, &enterEditor);
        QTest::qWait(150);
        QApplication::processEvents();
        QVERIFY(overlay.panel()->isVisible());
        QCOMPARE(log.height(), viewportHeight);
        QCOMPARE(editor.mapTo(&host, QPoint()).y(), editorY);
        QCOMPARE(overlay.panel()->width(), host.width());
        QCOMPARE(overlay.panel()->x(), 0);
        QVERIFY(overlay.panel()->geometry().bottom() < editorY);
        auto* frame = qobject_cast<QFrame*>(overlay.panel());
        QVERIFY(frame);
        QCOMPARE(frame->frameShape(), QFrame::NoFrame);
        QCOMPARE(frame->backgroundRole(), QPalette::Window);
        QVERIFY(frame->autoFillBackground());
        QCOMPARE(frame->layout()->contentsMargins(), QMargins());
        const int wideToolbarHeight = overlay.panel()->height();

        // Wrapping adds toolbar rows upward, not into the footer or log.
        host.resize(180, 400);
        QApplication::processEvents();
        QTest::qWait(20);
        QVERIFY(overlay.panel()->height() > wideToolbarHeight);
        QCOMPARE(overlay.panel()->width(), host.width());
        QCOMPARE(overlay.panel()->x(), 0);
        const int narrowViewportHeight = log.height();
        const int narrowEditorY = editor.mapTo(&host, QPoint()).y();

        QEvent leaveEditor(QEvent::Leave);
        QApplication::sendEvent(&editor, &leaveEditor);
        QEvent enterPanel(QEvent::Enter);
        QApplication::sendEvent(overlay.panel(), &enterPanel);
        QTest::qWait(150);
        QVERIFY(overlay.panel()->isVisible());
        overlay.setExpanded(false);
        QTest::qWait(150);
        QApplication::processEvents();
        QVERIFY(!overlay.panel()->isVisible());
        QCOMPARE(log.height(), narrowViewportHeight);
        QCOMPARE(editor.mapTo(&host, QPoint()).y(), narrowEditorY);

        // Focus returns with the pointer outside both regions: still hidden.
        overlay.setExpanded(true);
        QTest::qWait(150);
        QVERIFY(!overlay.panel()->isVisible());
        QApplication::sendEvent(&editor, &enterEditor);
        QTest::qWait(150);
        QApplication::processEvents();
        QVERIFY(overlay.panel()->isVisible());
        QCOMPARE(log.height(), narrowViewportHeight);
        QCOMPARE(editor.mapTo(&host, QPoint()).y(), narrowEditorY);

        // Some window managers deliver only the ChatArea's Leave when the
        // pointer crosses the native window edge, never QTextEdit::Leave.
        // A cached editor-hover state must not keep the overlay alive.
        QEvent leaveHost(QEvent::Leave);
        QApplication::sendEvent(&host, &leaveHost);
        QTest::qWait(150);
        QVERIFY(!overlay.panel()->isVisible());
        QCOMPARE(log.height(), narrowViewportHeight);
        QCOMPARE(editor.mapTo(&host, QPoint()).y(), narrowEditorY);
    }
};

QTEST_MAIN(FormattingToolbarOverlayTest)
#include "FormattingToolbarOverlayTest.moc"
