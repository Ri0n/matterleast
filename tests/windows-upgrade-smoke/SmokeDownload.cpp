#include "update/WindowsUpdateController.h"
#include "options/MLOptions.h"

#include <QCoreApplication>
#include <QDebug>
#include <QTimer>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("MatterLeastUpgradeSmoke"));
    application.setApplicationName(QStringLiteral("MatterLeastUpgradeSmoke"));
    application.setApplicationVersion(QStringLiteral("1.0"));

    // This executable is copied to versions/v1.0/MatterLeast/bin/matterleast.exe
    // in a disposable installation. The downloaded release is newer than 1.0.
    Mattermost::MLOptions::instance()->setValue(
        QStringLiteral("updates/channel"), QStringLiteral("stable"));
    Mattermost::MLOptions::instance()->setValue(
        QStringLiteral("updates/automatic"), false);

    Mattermost::WindowsUpdateController updater;
    if (updater.state() != Mattermost::WindowsUpdateController::State::Idle) {
        qCritical() << "Versioned test installation was not recognized";
        return 2;
    }

    bool receivedPayload = false;
    QObject::connect(&updater, &Mattermost::WindowsUpdateController::progressChanged,
                     &application, [&receivedPayload](qint64 current, qint64 total) {
        if (total > 0 && current > 0) receivedPayload = true;
    });
    QObject::connect(&updater, &Mattermost::WindowsUpdateController::errorOccurred,
                     &application, [&application](const QString& error) {
        qCritical().noquote() << "Update download/prepare failed:" << error;
        application.exit(3);
    });
    QObject::connect(&updater, &Mattermost::WindowsUpdateController::noUpdateAvailable,
                     &application, [&application](const QString& reason) {
        qCritical().noquote() << "Expected a newer release:" << reason;
        application.exit(4);
    });
    QObject::connect(&updater, &Mattermost::WindowsUpdateController::updatePrepared,
                     &application, [&application, &receivedPayload](const QString& id) {
        if (!receivedPayload) {
            qCritical() << "Update was prepared without an observed streamed download";
            application.exit(5);
            return;
        }
        qInfo().noquote() << "Downloaded and prepared version:" << id;
        application.exit(0);
    });
    QTimer::singleShot(0, &updater, &Mattermost::WindowsUpdateController::checkNow);
    QTimer::singleShot(240000, &application, [&application] {
        qCritical() << "Timed out waiting for download and ZIP preparation";
        application.exit(6);
    });
    return application.exec();
}
