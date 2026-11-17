#include <QtTest>

#include <QDialog>
#include <QPointer>
#include <QWidget>

#include "ui/TrayWindowController.h"

using namespace Mattermost;

class TrayWindowControllerTest : public QObject
{
    Q_OBJECT

private slots:
    void rejectedAutoDeletingLoginDoesNotLeaveDanglingTrayTarget()
    {
        TrayWindowController controller;
        auto* login = new QDialog;
        login->setAttribute(Qt::WA_DeleteOnClose);
        QPointer<QDialog> guard(login);
        controller.setWindow(login);

        login->open();
        QVERIFY(controller.showWindow());
        QCOMPARE(controller.window(), login);

        // This matches LoginDialog's WA_DeleteOnClose behavior. After
        // destruction, tray activation must not access freed QWidget data.
        login->reject();
        QTRY_VERIFY(guard.isNull());
        QVERIFY(controller.window() == nullptr);
        QVERIFY(!controller.toggleWindow());
        QVERIFY(!controller.showWindow());

        auto* replacement = new QDialog;
        controller.setWindow(replacement);
        QVERIFY(controller.showWindow());
        QCOMPARE(controller.window(), replacement);
        delete replacement;
        QVERIFY(controller.window() == nullptr);
        QVERIFY(!controller.toggleWindow());
    }

    void toggleAStillAliveWindow()
    {
        TrayWindowController controller;
        QWidget window;
        controller.setWindow(&window);
        QVERIFY(controller.toggleWindow());
        QVERIFY(window.isVisible());
        QVERIFY(controller.toggleWindow());
        QVERIFY(!window.isVisible());
        QVERIFY(controller.showWindow());
        QVERIFY(window.isVisible());
    }
};

QTEST_MAIN(TrayWindowControllerTest)
#include "TrayWindowControllerTest.moc"
