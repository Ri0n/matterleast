#include <QtTest>

#include <QJsonDocument>
#include <QCheckBox>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QTcpServer>
#include <QTcpSocket>
#include <QStandardPaths>

#include "backend/Backend.h"
#include "login/LoginDialog.h"
#include "backend/AccountManager.h"
#include "migrations/MigrationManager.h"
#include "backend/LoginResponsePolicy.h"
#include "backend/Storage.h"
#include "backend/types/BackendLoginData.h"
#include "options/MLOptions.h"

using namespace Mattermost;

class LoginHttpServer final : public QTcpServer
{
public:
    bool offline = true;
    bool holdResponses = false;
    int identityRequests = 0;

    LoginHttpServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray bytes = socket->property("received").toByteArray()
                        + socket->readAll();
                    socket->setProperty("received", bytes);
                    if (!bytes.contains("\r\n\r\n") ||
                        socket->property("responded").toBool()) return;
                    socket->setProperty("responded", true);
                    // The successful login also requests preferences and
                    // may initiate other API calls. Count only the identity
                    // request that this test intends to verify.
                    if (bytes.startsWith("GET /api/v4/users/me HTTP/")) {
                        ++identityRequests;
                    }
                    if (holdResponses) return;

                    const QByteArray body = offline
                        ? QByteArrayLiteral("{}")
                        : QByteArrayLiteral(
                            "{\"id\":\"user-id\",\"username\":\"test-user\"}");
                    const QByteArray status = offline
                        ? QByteArrayLiteral("503 Service Unavailable")
                        : QByteArrayLiteral("200 OK");
                    socket->write("HTTP/1.1 " + status
                        + "\r\nContent-Type: application/json\r\n"
                          "Connection: close\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
};

class LoginBootstrapTest final : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        // The native Windows settings backend requires a concrete
        // application/organization identity. Mirror the real application's
        // initialization and keep tests separate from user settings.
        QCoreApplication::setOrganizationName(QStringLiteral("matterleast-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("login-bootstrap-test"));
        QStandardPaths::setTestModeEnabled(true);
    }

    void rejectsFailedAndIdentityFreeResponses()
    {
        const QJsonDocument empty(QJsonObject{});
        const QJsonDocument valid(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("me")}
        });
        const auto networkFailure = validateLoginResponse(
            QNetworkReply::HostNotFoundError, 0, empty);
        QVERIFY(!networkFailure.accepted);
        QVERIFY(networkFailure.retryable);
        QVERIFY(!validateLoginResponse(QNetworkReply::NoError, 200, empty).accepted);
        const auto rejected = validateLoginResponse(
            QNetworkReply::ContentAccessDenied, 401, empty);
        QVERIFY(!rejected.accepted);
        QVERIFY(!rejected.retryable);
        QVERIFY(validateLoginResponse(QNetworkReply::InternalServerError, 500, empty).retryable);
        QVERIFY(validateLoginResponse(QNetworkReply::NoError, 200, valid).accepted);
    }

    void migrationVersionRegistryAndIncompleteLegacyPreservation()
    {
        int count = 0;
        const auto* steps = MigrationManager::steps(&count);
        QCOMPARE(count, MigrationManager::LatestVersion);
        QCOMPARE(steps[0].version, 1);
        QCOMPARE(QString::fromLatin1(steps[0].introducedOn),
                 QStringLiteral("2026-11-22"));
        QCOMPARE(QString::fromLatin1(steps[0].name),
                 QStringLiteral("legacy-single-account-login"));

        auto* settings = MLOptions::instance();
        const QStringList keys {QStringLiteral("domain"),
                                QStringLiteral("username"),
                                QStringLiteral("token")};
        QVector<QString> previous;
        for (const QString& key : keys)
            previous.append(settings->value<QString>(key));
        const int previousVersion = settings->value<int>(
            QStringLiteral("migrations/version"), 0);

        AccountManager accounts;
        MigrationManager migrations(accounts);
        settings->setValue<int>(QStringLiteral("migrations/version"), 0);
        settings->setValue<QString>(QStringLiteral("domain"), QString());
        settings->setValue<QString>(QStringLiteral("username"), QString());
        settings->setValue<QString>(QStringLiteral("token"), QStringLiteral("secret"));
        MigrationManager::Result result = MigrationManager::Result::Complete;
        migrations.run([&](MigrationManager::Result next) { result = next; });
        QVERIFY(result == MigrationManager::Result::ManualSignInRequired);
        QCOMPARE(settings->value<int>(QStringLiteral("migrations/version")), 0);
        QCOMPARE(settings->value<QString>(QStringLiteral("token")),
                 QStringLiteral("secret"));

        // With no legacy source, migration completes immediately and the
        // stored version advances; restarting doesn't mutate it.
        settings->setValue<QString>(QStringLiteral("token"), {});
        settings->setValue<int>(QStringLiteral("migrations/version"), 0);
        migrations.run([&](MigrationManager::Result next) { result = next; });
        QVERIFY(result == MigrationManager::Result::Complete);
        QCOMPARE(settings->value<int>(QStringLiteral("migrations/version")), 1);
        migrations.run([&](MigrationManager::Result next) { result = next; });
        QVERIFY(result == MigrationManager::Result::Complete);
        QCOMPARE(settings->value<int>(QStringLiteral("migrations/version")), 1);

        // Newer settings must never be downgraded or rewritten.
        settings->setValue<int>(QStringLiteral("migrations/version"), 999);
        migrations.run([&](MigrationManager::Result next) { result = next; });
        QVERIFY(result == MigrationManager::Result::UnsupportedVersion);
        QCOMPARE(settings->value<int>(QStringLiteral("migrations/version")), 999);
        QVERIFY(settings->value<QString>(QStringLiteral("token")).isEmpty());

        for (int i = 0; i < keys.size(); ++i)
            settings->setValue<QString>(keys[i], previous[i]);
        settings->setValue<int>(QStringLiteral("migrations/version"),
                                previousVersion);
    }

    void additionalAccountLoginNeverAutoRestoresLastUsedToken()
    {
        auto* settings = MLOptions::instance();
        const QString oldDomain = settings->value<QString>(QStringLiteral("domain"));
        const QString oldUsername = settings->value<QString>(QStringLiteral("username"));
        const QString oldToken = settings->value<QString>(QStringLiteral("token"));
        settings->setValue(QStringLiteral("domain"),
                           QStringLiteral("https://other.example.test/"));
        settings->setValue(QStringLiteral("username"),
                           QStringLiteral("previous-account"));
        settings->setValue(QStringLiteral("token"),
                           QStringLiteral("old-saved-token"));

        Backend second;
        {
            LoginDialog addDialog(nullptr, second);
            auto* host = addDialog.findChild<QLineEdit*>(
                QStringLiteral("domain_lineEdit"));
            auto* progress = addDialog.findChild<QLabel*>(
                QStringLiteral("loginInProgressLabel"));
            QVERIFY(host);
            QVERIFY(progress);
            QVERIFY(host->isEnabled());
            QVERIFY(progress->isHidden());
            QVERIFY(!second.loggedIn());
            QVERIFY(second.networkSession().host().isEmpty());
        }
        settings->setValue(QStringLiteral("domain"), oldDomain);
        settings->setValue(QStringLiteral("username"), oldUsername);
        settings->setValue(QStringLiteral("token"), oldToken);
    }

    void reauthenticationEditorIsBoundToSavedAccount()
    {
        auto* settings = MLOptions::instance();
        const QString oldDomain = settings->value<QString>(QStringLiteral("domain"));
        const QString oldUser = settings->value<QString>(QStringLiteral("username"));
        Backend backend;
        LoginDialog editor(nullptr, backend,
                           QStringLiteral("https://work.example.org/"),
                           QStringLiteral("alice@example.org"), true);
        auto* domain = editor.findChild<QLineEdit*>(QStringLiteral("domain_lineEdit"));
        auto* user = editor.findChild<QLineEdit*>(QStringLiteral("username_lineEdit"));
        auto* password = editor.findChild<QLineEdit*>(QStringLiteral("password_lineEdit"));
        auto* tokenMode = editor.findChild<QCheckBox*>(
            QStringLiteral("personalAccessTokenCheck"));
        QVERIFY(domain);
        QVERIFY(user);
        QVERIFY(password);
        QVERIFY(tokenMode);
        QCOMPARE(domain->text(), QStringLiteral("https://work.example.org/"));
        QCOMPARE(user->text(), QStringLiteral("alice@example.org"));
        QVERIFY(domain->isReadOnly());
        QVERIFY(user->isReadOnly());
        QVERIFY(!tokenMode->isChecked());
        tokenMode->setChecked(true);
        QCOMPARE(password->placeholderText(), QStringLiteral("Personal access token"));
        QVERIFY(password->echoMode() == QLineEdit::Password);
        QCOMPARE(settings->value<QString>(QStringLiteral("domain")), oldDomain);
        QCOMPARE(settings->value<QString>(QStringLiteral("username")), oldUser);
        QVERIFY(!backend.loggedIn());
    }

    void canCancelInFlightSavedSessionAndConnectToAnotherServer()
    {
        LoginHttpServer stalled;
        LoginHttpServer replacement;
        stalled.holdResponses = true;
        replacement.offline = false;
        QVERIFY(stalled.listen(QHostAddress::LocalHost));
        QVERIFY(replacement.listen(QHostAddress::LocalHost));

        Backend backend;
        BackendLoginData oldLogin;
        oldLogin.domain = QStringLiteral("http://127.0.0.1:%1/")
            .arg(stalled.serverPort());
        oldLogin.username = QStringLiteral("old-user");
        oldLogin.token = QStringLiteral("old-session-token");

        bool canceledCallbackInvoked = false;
        backend.login(oldLogin, [&](const QString&) {
            canceledCallbackInvoked = true;
        });
        QTRY_COMPARE(stalled.identityRequests, 1);
        backend.cancelPendingLogin();
        QVERIFY(!backend.loggedIn());

        BackendLoginData newLogin;
        newLogin.domain = QStringLiteral("http://127.0.0.1:%1/")
            .arg(replacement.serverPort());
        newLogin.username = QStringLiteral("new-user");
        newLogin.token = QStringLiteral("new-session-token");
        bool completed = false;
        QString resultToken;
        backend.login(newLogin, [&](const QString& token) {
            resultToken = token;
            completed = true;
        });
        QTRY_VERIFY(completed);
        QCOMPARE(resultToken, newLogin.token);
        QCOMPARE(replacement.identityRequests, 1);
        QVERIFY(!canceledCallbackInvoked);
        QVERIFY(backend.getStorage().loginUser);
        QCOMPARE(backend.getStorage().loginUser->id,
                 QStringLiteral("user-id"));
    }

    void doesNotEnterMainWindowWithEmptyProfileAndRecoversOnRetry()
    {
        LoginHttpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        Backend backend;
        BackendLoginData credentials;
        credentials.domain = QStringLiteral("http://127.0.0.1:%1/")
            .arg(server.serverPort());
        credentials.username = QStringLiteral("test-user");
        credentials.token = QStringLiteral("saved-token");

        bool firstFinished = false;
        QString firstToken;
        backend.login(credentials, [&](const QString& token) {
            firstFinished = true;
            firstToken = token;
        });
        QTRY_VERIFY(firstFinished);
        QVERIFY(firstToken.isEmpty());
        QVERIFY(backend.getStorage().loginUser == nullptr);
        QVERIFY(backend.lastLoginFailureTransient());

        server.offline = false;
        bool secondFinished = false;
        QString secondToken;
        backend.login(credentials, [&](const QString& token) {
            secondFinished = true;
            secondToken = token;
        });
        QTRY_VERIFY(secondFinished);
        QCOMPARE(secondToken, credentials.token);
        QVERIFY(backend.getStorage().loginUser);
        QCOMPARE(backend.getStorage().loginUser->id,
                 QStringLiteral("user-id"));
        QVERIFY(!backend.lastLoginFailureTransient());
    }
};

QTEST_MAIN(LoginBootstrapTest)
#include "LoginBootstrapTest.moc"
