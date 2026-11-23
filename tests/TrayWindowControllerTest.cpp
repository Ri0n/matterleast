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
        QCOMPARE(controller.window(), static_cast<QWidget*>(login));

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
        QCOMPARE(controller.window(), static_cast<QWidget*>(replacement));
        delete replacement;
        QVERIFY(controller.window() == nullptr);
        QVERIFY(!controller.toggleWindow());
    }

    void logoutCannotReactivateAnOldButStillAliveMainWindow()
    {
        TrayWindowController controller;
        QWidget mainWindow;
        controller.setWindow(&mainWindow);
        QVERIFY(controller.showWindow());

        // Session teardown first removes the tray target, while the old
        // widgets and all referenced Backend::Storage objects are alive.
        controller.setWindow(nullptr);
        mainWindow.hide();
        QVERIFY(!controller.showWindow());
        QVERIFY(!controller.toggleWindow());
        QVERIFY(!mainWindow.isVisible());

        // The replacement target is established only after session reset.
        QWidget login;
        controller.setWindow(&login);
        QVERIFY(controller.showWindow());
        QVERIFY(login.isVisible());
        QVERIFY(!mainWindow.isVisible());
    }

    void preservesGeometryAcrossTrayToggleAndWindowClose()
    {
        TrayWindowController controller;
        QWidget window;
        window.resize(1100, 730);
        controller.setWindow(&window);
        QVERIFY(controller.showWindow());
        QCOMPARE(window.size(), QSize(1100, 730));

        // Simulate a compositor assigning its minimum size after unmapping.
        QVERIFY(controller.toggleWindow());
        QVERIFY(!window.isVisible());
        window.resize(180, 130);
        QVERIFY(controller.toggleWindow());
        QVERIFY(window.isVisible());
        QCOMPARE(window.size(), QSize(1100, 730));

        // Closing a plain QWidget bypasses toggleWindow(). The hide event
        // must still remember its last size for the next tray activation.
        window.resize(1010, 690);
        QVERIFY(window.close());
        QVERIFY(!window.isVisible());
        window.resize(210, 160);
        QVERIFY(controller.showWindow());
        QCOMPARE(window.size(), QSize(1010, 690));
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
