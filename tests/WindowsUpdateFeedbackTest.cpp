#include <QtTest>

#include "update/WindowsUpdateController.h"
#include "options/MLOptions.h"

#ifdef Q_OS_WIN

using Mattermost::WindowsUpdateController;

class WindowsUpdateFeedbackTest final : public QObject
{
    Q_OBJECT
private slots:
    void manualCheckWithoutVersionedInstallReportsError();
    void lastCheckIsTrackedPerChannel();
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

void WindowsUpdateFeedbackTest::lastCheckIsTrackedPerChannel()
{
    auto* options = Mattermost::MLOptions::instance();
    // Do not depend on an actual network endpoint, a user's existing
    // settings or an installed launcher. The dialog is a state viewer and
    // its history must be readable even when the updater is unavailable.
    const QString previous = options->value<QString>(
        QStringLiteral("updates/channel"), QStringLiteral("stable"));
    options->setValue(QStringLiteral("updates/channel"), QStringLiteral("stable"));
    const QString oldStable = options->value<QString>(
        QStringLiteral("updates/lastCheck/stable/result"));
    const QString oldNightly = options->value<QString>(
        QStringLiteral("updates/lastCheck/nightly/result"));
    const QString oldStableTime = options->value<QString>(
        QStringLiteral("updates/lastCheck/stable/time"));
    const QString oldNightlyTime = options->value<QString>(
        QStringLiteral("updates/lastCheck/nightly/time"));

    options->setValue(QStringLiteral("updates/lastCheck/stable/result"),
                      QStringLiteral("Stable: no newer version"));
    options->setValue(QStringLiteral("updates/lastCheck/stable/time"),
                      QStringLiteral("2026-10-28T09:00:00Z"));
    options->setValue(QStringLiteral("updates/lastCheck/nightly/result"),
                      QStringLiteral("Nightly: available"));
    options->setValue(QStringLiteral("updates/lastCheck/nightly/time"),
                      QStringLiteral("2026-10-28T10:00:00Z"));

    WindowsUpdateController updater;
    QCOMPARE(updater.state(), WindowsUpdateController::State::Unavailable);
    QCOMPARE(updater.checkChannel(), QStringLiteral("stable"));
    QCOMPARE(updater.lastCheckResult(), QStringLiteral("Stable: no newer version"));
    QVERIFY(updater.lastCheckTime().isValid());

    options->setValue(QStringLiteral("updates/channel"), QStringLiteral("nightly"));
    QCOMPARE(updater.checkChannel(), QStringLiteral("nightly"));
    QCOMPARE(updater.lastCheckResult(), QStringLiteral("Nightly: available"));
    QVERIFY(updater.lastCheckTime() > QDateTime::fromString(
        QStringLiteral("2026-10-28T09:00:00Z"), Qt::ISODate));
    QVERIFY(!updater.isBusy());

    options->setValue(QStringLiteral("updates/lastCheck/stable/result"), oldStable);
    options->setValue(QStringLiteral("updates/lastCheck/nightly/result"), oldNightly);
    options->setValue(QStringLiteral("updates/lastCheck/stable/time"), oldStableTime);
    options->setValue(QStringLiteral("updates/lastCheck/nightly/time"), oldNightlyTime);
    options->setValue(QStringLiteral("updates/channel"), previous);
}

QTEST_APPLESS_MAIN(WindowsUpdateFeedbackTest)
#include "WindowsUpdateFeedbackTest.moc"

#endif
