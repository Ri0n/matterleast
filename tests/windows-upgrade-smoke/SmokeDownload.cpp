#include "update/WindowsUpdateController.h"
#include "options/MLOptions.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QDir>
#include <cstdio>

namespace {
QFile* smokeLog = nullptr;
void logMessage(QtMsgType, const QMessageLogContext&, const QString& message)
{
    const QByteArray line = message.toUtf8() + '\n';
    if (smokeLog) {
        smokeLog->write(line);
        smokeLog->flush();
    }
    std::fwrite(line.constData(), 1, size_t(line.size()), stderr);
    std::fflush(stderr);
}
} // namespace
#include <QTimer>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    QFile outputLog(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("smoke.log")));
    if (!outputLog.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 25;
    smokeLog = &outputLog;
    qInstallMessageHandler(logMessage);
    application.setOrganizationName(QStringLiteral("MatterLeastUpgradeSmoke"));
    application.setApplicationName(QStringLiteral("MatterLeastUpgradeSmoke"));
    application.setApplicationVersion(QStringLiteral("1.0"));

    // This executable is copied to versions/v1.0/MatterLeast/bin/matterleast.exe
    // in a disposable installation. The current nightly release is newer than 1.0.
    Mattermost::MLOptions::instance()->setValue(
        QStringLiteral("updates/channel"), QStringLiteral("nightly"));
    Mattermost::MLOptions::instance()->setValue(
        QStringLiteral("updates/automatic"), false);

    Mattermost::WindowsUpdateController updater;
    if (updater.state() != Mattermost::WindowsUpdateController::State::Idle) {
        qCritical() << "Versioned test installation was not recognized";
        return 2;
    }

    QObject::connect(&updater, &Mattermost::WindowsUpdateController::stateChanged,
                     &application, [](Mattermost::WindowsUpdateController::State state) {
        qInfo() << "Updater state:" << int(state);
    });
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
