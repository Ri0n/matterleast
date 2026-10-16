#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QStandardPaths>

#include "backend/Backend.h"
#include "backend/LoginResponsePolicy.h"
#include "backend/Storage.h"
#include "backend/types/BackendLoginData.h"

using namespace Mattermost;

class LoginHttpServer final : public QTcpServer
{
public:
    bool offline = true;
    int requests = 0;

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
                    ++requests;

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
        QVERIFY(validateLoginResponse(QNetworkReply::NoError, 200, valid).accepted);
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
