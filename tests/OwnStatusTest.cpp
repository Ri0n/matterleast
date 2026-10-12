#include <QtTest>

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>

#include "backend/Backend.h"
#include "backend/CustomStatusValidation.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "backend/types/BackendUser.h"

using namespace Mattermost;

class OwnStatusTest final : public QObject
{
    Q_OBJECT
private slots:
    void emojiOnlyAndEmptyStatusContent()
    {
        QVERIFY(hasCustomStatusContent(QStringLiteral("coffee"), QString()));
        QVERIFY(hasCustomStatusContent(QString(), QStringLiteral("Working")));
        QVERIFY(hasCustomStatusContent(QStringLiteral("coffee"), QStringLiteral("Working")));
        QVERIFY(!hasCustomStatusContent(QString(), QString()));
        QVERIFY(!hasCustomStatusContent(QStringLiteral("  "), QStringLiteral("  ")));
    }

    void statusEndpointsAndFailedUpdate()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/")
                                    .arg(server.serverPort()));

        int statusCode = 200;
        QByteArray lastMethod, lastPath, lastBody;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create();
                connect(socket, &QTcpSocket::readyRead, socket,
                        [&, socket, buffer] {
                    buffer->append(socket->readAll());
                    const int end = buffer->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray headers = buffer->left(end);
                    int contentLength = 0;
                    for (const QByteArray& header : headers.split('\n')) {
                        if (header.trimmed().toLower().startsWith("content-length:")) {
                            contentLength = header.mid(header.indexOf(':') + 1)
                                                .trimmed().toInt();
                        }
                    }
                    if (buffer->size() < end + 4 + contentLength) return;
                    const QList<QByteArray> requestParts = headers.split(' ');
                    if (requestParts.size() >= 2) {
                        lastMethod = requestParts.at(0);
                        lastPath = requestParts.at(1);
                    }
                    lastBody = buffer->mid(end + 4, contentLength);
                    socket->write(
                        QByteArray("HTTP/1.1 ") + QByteArray::number(statusCode)
                        + (statusCode == 200 ? " OK\r\n" : " Internal Server Error\r\n")
                        + "Content-Type: application/json\r\n"
                          "Content-Length: 2\r\nConnection: close\r\n\r\n{}");
                    socket->flush();
                    socket->disconnectFromHost();
                });
            }
        });

        Backend backend;
        BackendUser* user = backend.getStorage().addUser(
            QJsonObject {{QStringLiteral("id"), QStringLiteral("user-1")},
                         {QStringLiteral("username"), QStringLiteral("user")}},
            true);
        QVERIFY(user);
        user->status = QStringLiteral("online");
        QSignalSpy availabilitySignal(user, &BackendUser::onStatusChanged);
        QSignalSpy customSignal(&backend, &Backend::onOwnCustomStatusChanged);

        bool finished = false, succeeded = false;
        backend.setOwnAvailability(QStringLiteral("away"),
            [&](bool ok, const QString&) { succeeded = ok; finished = true; });
        QTRY_VERIFY(finished);
        QVERIFY(succeeded);
        QCOMPARE(lastMethod, QByteArray("PUT"));
        QVERIFY(lastPath.endsWith("/users/user-1/status"));
        QCOMPARE(QJsonDocument::fromJson(lastBody).object()
                     .value(QStringLiteral("status")).toString(),
                 QStringLiteral("away"));
        QCOMPARE(user->status, QStringLiteral("away"));
        QCOMPARE(availabilitySignal.size(), 1);

        finished = false;
        backend.setOwnCustomStatus(QStringLiteral("coffee"),
            QStringLiteral("Concentrating"), QStringLiteral("one_hour"), {},
            [&](bool ok, const QString&) { succeeded = ok; finished = true; });
        QTRY_VERIFY(finished);
        QVERIFY(succeeded);
        QCOMPARE(lastMethod, QByteArray("PUT"));
        QVERIFY(lastPath.endsWith("/users/user-1/status/custom"));
        QCOMPARE(QJsonDocument::fromJson(lastBody).object()
                     .value(QStringLiteral("duration")).toString(),
                 QStringLiteral("one_hour"));
        QCOMPARE(user->props.customStatus.text, QStringLiteral("Concentrating"));
        QCOMPARE(customSignal.size(), 1);

        // Mattermost allows a status with just an emoji and no text.
        finished = false;
        backend.setOwnCustomStatus(QStringLiteral("coffee"), QString(),
            QStringLiteral("one_hour"), {},
            [&](bool ok, const QString&) { succeeded = ok; finished = true; });
        QTRY_VERIFY(finished);
        QVERIFY(succeeded);
        QCOMPARE(lastMethod, QByteArray("PUT"));
        QCOMPARE(QJsonDocument::fromJson(lastBody).object()
                     .value(QStringLiteral("emoji")).toString(),
                 QStringLiteral("coffee"));
        QCOMPARE(QJsonDocument::fromJson(lastBody).object()
                     .value(QStringLiteral("text")).toString(), QString());
        QCOMPARE(user->props.customStatus.emoji, QStringLiteral("coffee"));
        QVERIFY(user->props.customStatus.text.isEmpty());
        QCOMPARE(customSignal.size(), 2);

        statusCode = 500;
        finished = false;
        backend.setOwnAvailability(QStringLiteral("dnd"),
            [&](bool ok, const QString&) { succeeded = ok; finished = true; });
        QTRY_VERIFY(finished);
        QVERIFY(!succeeded);
        QCOMPARE(user->status, QStringLiteral("away"));
        QCOMPARE(availabilitySignal.size(), 1);

        statusCode = 200;
        finished = false;
        backend.clearOwnCustomStatus(
            [&](bool ok, const QString&) { succeeded = ok; finished = true; });
        QTRY_VERIFY(finished);
        QVERIFY(succeeded);
        QCOMPARE(lastMethod, QByteArray("DELETE"));
        QVERIFY(lastPath.endsWith("/users/user-1/status/custom"));
        QVERIFY(user->props.customStatus.text.isEmpty());
        QCOMPARE(customSignal.size(), 3);
        QVERIFY(user->props.customStatus.emoji.isEmpty());
    }
};

QTEST_MAIN(OwnStatusTest)
#include "OwnStatusTest.moc"
