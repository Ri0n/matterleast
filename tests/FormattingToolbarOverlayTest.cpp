#include <QApplication>
#include <QFrame>
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

        FormattingToolbarOverlay overlay(host, editor, toolbar);

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
    }
};

QTEST_MAIN(FormattingToolbarOverlayTest)
#include "FormattingToolbarOverlayTest.moc"
