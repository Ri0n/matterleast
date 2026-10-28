#include <QtTest>

#include "update/WindowsUpdateController.h"

#ifdef Q_OS_WIN

using Mattermost::WindowsUpdateController;

class WindowsUpdateFeedbackTest final : public QObject
{
    Q_OBJECT
private slots:
    void manualCheckWithoutVersionedInstallReportsError();
};

void WindowsUpdateFeedbackTest::manualCheckWithoutVersionedInstallReportsError()
{
    // The test binary is not installed inside versions/<id>/MatterLeast/bin.
    // This is the same unsupported layout that previously produced no
    // response whatsoever from the Help → Check for updates action.
    WindowsUpdateController updater;
    QCOMPARE(updater.state(), WindowsUpdateController::State::Unavailable);

    QSignalSpy started(&updater, &WindowsUpdateController::manualCheckStarted);
    QSignalSpy errors(&updater, &WindowsUpdateController::errorOccurred);
    QSignalSpy noUpdates(&updater, &WindowsUpdateController::noUpdateAvailable);
    QVERIFY(started.isValid());
    QVERIFY(errors.isValid());
    QVERIFY(noUpdates.isValid());

    updater.checkNow();
    QCOMPARE(errors.size(), 1);
    QCOMPARE(started.size(), 0);
    QCOMPARE(noUpdates.size(), 0);
    const QString explanation = errors.at(0).at(0).toString();
    QVERIFY2(explanation.contains(QStringLiteral("versioned installation")),
             qPrintable(explanation));
    QVERIFY2(explanation.contains(QStringLiteral("MatterLeastLauncher.exe")),
             qPrintable(explanation));

    // A subsequent manual attempt must also report an error instead of
    // entering an unresponsive state.
    updater.checkNow();
    QCOMPARE(errors.size(), 2);
}

QTEST_APPLESS_MAIN(WindowsUpdateFeedbackTest)
#include "WindowsUpdateFeedbackTest.moc"

#endif
