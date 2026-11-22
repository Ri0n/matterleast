#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocket>
#include <QWebSocketServer>

#include "backend/Backend.h"
#include "backend/WebSocketConnector.h"
#include "backend/WebSocketEventHandler.h"

using namespace Mattermost;

class WebSocketConfigServer final : public QTcpServer
{
public:
    explicit WebSocketConfigServer(quint16 socketPort)
        : _socketPort(socketPort)
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray request = socket->property("request").toByteArray()
                        + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n")
                        || socket->property("answered").toBool())
                        return;
                    socket->setProperty("answered", true);
                    const QJsonDocument config(QJsonObject{
                        {QStringLiteral("WebsocketURL"),
                         QStringLiteral("ws://127.0.0.1:%1").arg(_socketPort)}
                    });
                    const QByteArray body = config.toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\n"
                                  "Content-Type: application/json\r\n"
                                  "Connection: close\r\n"
                                  "Content-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }
private:
    quint16 _socketPort;
};

class WebSocketContinuityTest final : public QObject
{
    Q_OBJECT
private slots:
    void authenticatedHelloAndReplayAreRequired()
    {
        QStandardPaths::setTestModeEnabled(true);
        QWebSocketServer server(QStringLiteral("stream-test"),
                                QWebSocketServer::NonSecureMode);
        QVERIFY(server.listen(QHostAddress::LocalHost));
        WebSocketConfigServer config(server.serverPort());
        QVERIFY(config.listen(QHostAddress::LocalHost));

        Backend backend;
        WebSocketEventHandler handler(backend);
        WebSocketConnector connector(handler);
        QSignalSpy connected(&connector, &WebSocketConnector::onConnect);
        QVector<QWebSocket*> sockets;
        QHash<QWebSocket*, int> authenticationSequences;

        connect(&server, &QWebSocketServer::newConnection, this, [&] {
            while (server.hasPendingConnections()) {
                QWebSocket* socket = server.nextPendingConnection();
                sockets.append(socket);
                connect(socket, &QWebSocket::textMessageReceived, this,
                        [&, socket](const QString& text) {
                    const QJsonObject command =
                        QJsonDocument::fromJson(text.toUtf8()).object();
                    const QString action =
                        command.value(QStringLiteral("action")).toString();
                    const int seq = command.value(QStringLiteral("seq")).toInt();
                    if (action == QLatin1String("authentication_challenge")) {
                        authenticationSequences.insert(socket, seq);
                    } else if (action == QLatin1String("ping")) {
                        socket->sendTextMessage(QString::fromUtf8(
                            QJsonDocument(QJsonObject{
                                {QStringLiteral("seq_reply"), seq}
                            }).toJson(QJsonDocument::Compact)));
                    }
                });
            }
        });

        const auto hello = [](QWebSocket* socket, const QString& id, int seq) {
            const QJsonDocument event(QJsonObject{
                {QStringLiteral("event"), QStringLiteral("hello")},
                {QStringLiteral("seq"), seq},
                {QStringLiteral("data"), QJsonObject{
                    {QStringLiteral("connection_id"), id}
                }}
            });
            socket->sendTextMessage(
                QString::fromUtf8(event.toJson(QJsonDocument::Compact)));
        };
        const auto acceptAuth = [&](QWebSocket* socket) {
            const QJsonDocument reply(QJsonObject{
                {QStringLiteral("seq_reply"), authenticationSequences.value(socket)},
                {QStringLiteral("status"), QStringLiteral("OK")}
            });
            socket->sendTextMessage(
                QString::fromUtf8(reply.toJson(QJsonDocument::Compact)));
        };

        connector.open(QStringLiteral("http://127.0.0.1:%1/api/v4/")
                           .arg(config.serverPort()), QStringLiteral("token"));
        QTRY_COMPARE(sockets.size(), 1);
        QWebSocket* first = sockets[0];
        QTRY_VERIFY(authenticationSequences.contains(first));
        hello(first, QStringLiteral("stream-a"), 0);
        QTest::qWait(100);
        QCOMPARE(connected.size(), 0); // hello alone is not authenticated
        acceptAuth(first);
        QTRY_COMPARE(connected.size(), 1);
        QCOMPARE(connected.at(0).at(0).toBool(), false);
        QCOMPARE(connected.at(0).at(1).toBool(), false);

        first->close();
        QTRY_VERIFY(connector.connectionState()
                    == WebSocketConnector::ConnectionState::WaitingForReconnect);
        connector.reconnectNow();
        QTRY_COMPARE(sockets.size(), 2);
        QWebSocket* second = sockets[1];
        QTRY_VERIFY(authenticationSequences.contains(second));
        // The same stream and contiguous sequence permit lossless replay.
        hello(second, QStringLiteral("stream-a"), 1);
        QTest::qWait(100);
        QCOMPARE(connected.size(), 1);
        acceptAuth(second);
        QTRY_COMPARE(connected.size(), 2);
        QCOMPARE(connected.at(1).at(0).toBool(), true);
        QCOMPARE(connected.at(1).at(1).toBool(), false);

        second->close();
        QTRY_VERIFY(connector.connectionState()
                    == WebSocketConnector::ConnectionState::WaitingForReconnect);
        connector.reconnectNow();
        QTRY_COMPARE(sockets.size(), 3);
        QWebSocket* third = sockets[2];
        QTRY_VERIFY(authenticationSequences.contains(third));
        // The server has discarded its buffered stream: a new connection id
        // is not a successful replay even with a valid auth token.
        hello(third, QStringLiteral("stream-b"), 0);
        acceptAuth(third);
        QTRY_COMPARE(connected.size(), 3);
        QCOMPARE(connected.at(2).at(0).toBool(), true);
        QCOMPARE(connected.at(2).at(1).toBool(), true);

        // A sequence discontinuity *after* handshake also invalidates replay.
        // The next request must start a fresh stream, not loop forever at
        // the previously acknowledged sequence number.
        third->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("posted")},
            {QStringLiteral("seq"), 7}
        }).toJson(QJsonDocument::Compact)));
        QTRY_VERIFY(connector.connectionState()
                    == WebSocketConnector::ConnectionState::WaitingForReconnect);
        connector.reconnectNow();
        QTRY_COMPARE(sockets.size(), 4);
        QWebSocket* fourth = sockets[3];
        QTRY_VERIFY(authenticationSequences.contains(fourth));
        hello(fourth, QStringLiteral("stream-c"), 0);
        acceptAuth(fourth);
        QTRY_COMPARE(connected.size(), 4);
        QCOMPARE(connected.at(3).at(0).toBool(), true);
        QCOMPARE(connected.at(3).at(1).toBool(), true);

        fourth->close();
        QTRY_VERIFY(connector.connectionState()
                    == WebSocketConnector::ConnectionState::WaitingForReconnect);
        connector.reconnectNow();
        QTRY_COMPARE(sockets.size(), 5);
        QWebSocket* fifth = sockets[4];
        QTRY_VERIFY(authenticationSequences.contains(fifth));
        // A resumed stream without hello must not get stuck in retries nor be
        // mistaken for a confirmed lossless replay.
        fifth->sendTextMessage(QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("event"), QStringLiteral("unknown_replay_event")},
            {QStringLiteral("seq"), 1}
        }).toJson(QJsonDocument::Compact)));
        QTest::qWait(100);
        QCOMPARE(connected.size(), 4);
        acceptAuth(fifth);
        QTRY_COMPARE(connected.size(), 5);
        QCOMPARE(connected.at(4).at(0).toBool(), true);
        QCOMPARE(connected.at(4).at(1).toBool(), true);

        connector.close();
        for (QWebSocket* socket : sockets) {
            socket->close();
            socket->deleteLater();
        }
    }
};

QTEST_MAIN(WebSocketContinuityTest)
#include "WebSocketContinuityTest.moc"
