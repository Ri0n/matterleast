#include "update/WindowsUpdateController.h"
#include "options/MLOptions.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QNetworkReply>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTimer>

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    application.setOrganizationName(QStringLiteral("MatterLeastSignedSmoke"));
    application.setApplicationName(QStringLiteral("MatterLeastSignedSmoke"));
    application.setApplicationVersion(QStringLiteral("1.0"));

    const QString expected = qEnvironmentVariable("MATTERLEAST_SIGNED_SMOKE_EXPECT");
    const auto certs = QSslCertificate::fromPath(
        qEnvironmentVariable("MATTERLEAST_SIGNED_SMOKE_TLS_CERT"));
    if (certs.size() != 1) {
        qCritical() << "Cannot load test HTTPS certificate";
        return 10;
    }
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    auto trusted = ssl.caCertificates();
    trusted.append(certs.first());
    ssl.setCaCertificates(trusted);
    QSslConfiguration::setDefaultConfiguration(ssl);

    Mattermost::MLOptions::instance()->setValue(
        QStringLiteral("updates/channel"), QStringLiteral("nightly"));
    Mattermost::MLOptions::instance()->setValue(
        QStringLiteral("updates/automatic"), false);

    Mattermost::WindowsUpdateController updater;
    if (updater.state() != Mattermost::WindowsUpdateController::State::Idle) {
        qCritical() << "Signed test installation was not recognized";
        return 11;
    }
    bool zipRequested = false;
    QObject::connect(&updater, &Mattermost::WindowsUpdateController::progressChanged,
                     &application, [&zipRequested](qint64 count, qint64) {
        if (count > 0) zipRequested = true;
    });
    QObject::connect(&updater, &Mattermost::WindowsUpdateController::updatePrepared,
                     &application, [&application, &zipRequested, expected](const QString& id) {
        if (expected != QLatin1String("ready") || !zipRequested) {
            qCritical() << "Unexpected preparation:" << id << expected;
            application.exit(12);
            return;
        }
        qInfo() << "Signed download and ZIP preparation accepted:" << id;
        application.exit(0);
    });
    QObject::connect(&updater, &Mattermost::WindowsUpdateController::errorOccurred,
                     &application, [&application, expected](const QString& error) {
        qInfo().noquote() << "Signed update failed:" << error;
        QString expectedText;
        if (expected == QLatin1String("signature"))
            expectedText = QStringLiteral("signature verification failed");
        else if (expected == QLatin1String("metadata"))
            expectedText = QStringLiteral("Could not fetch update metadata");
        else if (expected == QLatin1String("sha"))
            expectedText = QStringLiteral("SHA-256 mismatch");
        const bool matched = !expectedText.isEmpty()
            && error.contains(expectedText, Qt::CaseInsensitive);
        application.exit(matched ? 0 : 13);
    });
    QTimer::singleShot(0, &updater, &Mattermost::WindowsUpdateController::checkNow);
    QTimer::singleShot(60000, &application, [&application] {
        qCritical() << "Timed out waiting for signed update result";
        application.exit(14);
    });
    return application.exec();
}
