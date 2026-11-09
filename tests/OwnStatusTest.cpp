#include <QtTest>

#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/HTTPConnector.h"
#include "backend/CustomStatusValidation.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "ui/EmojiPresentation.h"
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
        QVERIFY(isEmojiOnlyCustomStatus(QStringLiteral("coffee"), QString()));
        QVERIFY(isEmojiOnlyCustomStatus(QStringLiteral("coffee"), QStringLiteral("  ")));
        QVERIFY(!isEmojiOnlyCustomStatus(QStringLiteral("coffee"),
                                         QStringLiteral("Working")));
        QVERIFY(!isEmojiOnlyCustomStatus(QString(), QStringLiteral("Working")));
        QVERIFY(!isEmojiOnlyCustomStatus(QStringLiteral("  "), QString()));
    }

    void emojiOnlyStatusHasTripleSize()
    {
        QFont standard;
        standard.setPointSize(10);
        const QFont large = EmojiPresentation::fontForMode(
            standard, EmojiPresentation::Mode::StatusJumbo);
        QCOMPARE(large.pointSizeF(), 3.0 * standard.pointSizeF());
        QCOMPARE(EmojiPresentation::extent(
                     standard, EmojiPresentation::Mode::Reaction), 16);
        QCOMPARE(EmojiPresentation::extent(
                     standard, EmojiPresentation::Mode::StatusJumbo), 48);
    }

    void reconnectWithMultiplePendingReplies()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/")
                                    .arg(server.serverPort()));

        QHash<QByteArray, int> attempts;
        int firstBatch = 0;
        bool restarting = false;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create();
                connect(socket, &QTcpSocket::readyRead, socket,
                        [&, socket, buffer] {
                    buffer->append(socket->readAll());
                    const int end = buffer->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray line = buffer->left(
                        buffer->indexOf("\r\n"));
                    const QByteArrayList parts = line.split(' ');
                    if (parts.size() < 2 || parts.at(0) != "GET") return;
                    ++attempts[parts.at(1)];

                    if (!restarting) {
                        ++firstBatch;
                        return;
                    }

                    const QByteArray body = QByteArrayLiteral("{\"ok\":true}");
                    socket->write(QByteArrayLiteral(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: ") + QByteArray::number(body.size())
                        + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body);
                    socket->flush();
                    socket->disconnectFromHost();
                });
            }
        });

        HTTPConnector first, second;
        int completed = 0;
        bool allSucceeded = true;
        for (int i = 0; i < 4; ++i) {
            NetworkRequest request(QStringLiteral("probe/%1").arg(i));
            auto callback = HttpResponseCallback(
                [&](const QJsonDocument& doc, const QNetworkReply& reply) {
                    allSucceeded = allSucceeded
                        && reply.error() == QNetworkReply::NoError
                        && doc.object().value(QStringLiteral("ok")).toBool();
                    ++completed;
                });
            if (i % 2 == 0) {
                first.get(request, callback);
            } else {
                second.get(request, callback);
            }
        }

        // All four QNetworkReply objects exist already, even when Qt has
        // not yet dispatched all of their HTTP/1.1 requests to the server.
        // Do not couple the restart trigger to the server's arrival order.
        QTimer::singleShot(200, &server, [&] {
            restarting = true;
            HTTPConnector::restartAllTransports();
        });

        QTRY_COMPARE_WITH_TIMEOUT(completed, 4, 10000);
        QVERIFY(allSucceeded);
        QVERIFY(firstBatch > 0);
        QCOMPARE(attempts.size(), 4);
        for (int i = 0; i < 4; ++i) {
            // Qt can queue requests behind an HTTP/1.1 connection, so only
            // requests that reached the mock server before restart are seen
            // twice. Every logical GET must still finish exactly once.
            const int count = attempts.value(
                QByteArrayLiteral("/api/v4/probe/")
                    + QByteArray::number(i));
            QVERIFY(count >= 1 && count <= 2);
        }
    }

    void ambiguousAvailabilityReconcilesAfterReconnect_data()
    {
        QTest::addColumn<QString>("authoritativeStatus");
        QTest::newRow("write-reached-server") << QStringLiteral("online");
        QTest::newRow("automatic-away-after-online") << QStringLiteral("away");
        QTest::newRow("write-not-applied") << QStringLiteral("offline");
    }

    void ambiguousAvailabilityReconcilesAfterReconnect()
    {
        QFETCH(QString, authoritativeStatus);

        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/")
                                    .arg(server.serverPort()));

        int putCount = 0;
        int getCount = 0;
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

                    const QByteArray method = headers.left(headers.indexOf(' '));
                    if (method == "PUT") {
                        ++putCount;
                        // The server may have applied the PUT, but the route
                        // disappears before the HTTP response reaches us.
                        // Deliberately leave this socket without a response.
                        QTimer::singleShot(0, &server, [] {
                            HTTPConnector::restartAllTransports();
                        });
                        return;
                    }
                    if (method == "GET") {
                        ++getCount;
                        const QByteArray body = QByteArrayLiteral(
                            "{\"user_id\":\"user-1\",\"status\":\"")
                            + authoritativeStatus.toUtf8() + QByteArrayLiteral("\"}");
                        socket->write(QByteArrayLiteral(
                            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                            "Content-Length: ") + QByteArray::number(body.size())
                            + QByteArrayLiteral("\r\nConnection: close\r\n\r\n") + body);
                        socket->flush();
                        socket->disconnectFromHost();
                    }
                });
            }
        });

        Backend backend;
        BackendUser* user = backend.getStorage().addUser(
            QJsonObject {{QStringLiteral("id"), QStringLiteral("user-1")},
                         {QStringLiteral("username"), QStringLiteral("user")}},
            true);
        QVERIFY(user);
        user->status = QStringLiteral("offline");
        QSignalSpy availabilitySpy(user, &BackendUser::onStatusChanged);

        bool finished = false, accepted = false;
        QString resultError;
        backend.setOwnAvailability(QStringLiteral("online"),
            [&](bool ok, const QString& error) {
                accepted = ok;
                resultError = error;
                finished = true;
            });
        QTRY_VERIFY_WITH_TIMEOUT(finished, 10000);
        QCOMPARE(putCount, 1); // Non-idempotent writes are never replayed.
        QCOMPARE(getCount, 1);
        QCOMPARE(accepted, authoritativeStatus != QStringLiteral("offline"));
        QCOMPARE(user->status, authoritativeStatus);
        QCOMPARE(availabilitySpy.size(),
                 authoritativeStatus == QStringLiteral("offline") ? 0 : 1);
        if (accepted) {
            QVERIFY(resultError.isEmpty());
        } else {
            QVERIFY(resultError.contains(QStringLiteral("server"),
                                         Qt::CaseInsensitive));
        }
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
