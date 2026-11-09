#include <QtTest>

#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include "backend/Backend.h"
#include "backend/WebSocketConnector.h"
#include "backend/WebSocketEventHandler.h"

using namespace Mattermost;

class WebSocketReconnectTest final : public QObject
{
    Q_OBJECT
private slots:
    void retriesFailedWebSocketHandshakeUntilExplicitClose()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString baseUrl = QStringLiteral("http://127.0.0.1:%1/api/v4/")
                                    .arg(server.serverPort());

        int configRequests = 0;
        int socketAttempts = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                auto buffer = QSharedPointer<QByteArray>::create();
                connect(socket, &QTcpSocket::readyRead, socket,
                        [&, socket, buffer] {
                    buffer->append(socket->readAll());
                    const int headerEnd = buffer->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;

                    const QByteArray firstLine =
                        buffer->left(buffer->indexOf("\r\n"));
                    if (firstLine.contains("/config/client")) {
                        ++configRequests;
                        socket->write(QByteArrayLiteral(
                            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                            "Content-Length: 2\r\nConnection: close\r\n\r\n{}"));
                    } else if (firstLine.contains("/websocket")) {
                        ++socketAttempts;
                        // The server never completes an upgrade. Depending on
                        // Qt/platform this path may emit errorOccurred but no
                        // disconnected signal: it was never ConnectedState.
                        socket->write(QByteArrayLiteral(
                            "HTTP/1.1 503 Service Unavailable\r\n"
                            "Content-Length: 0\r\nConnection: close\r\n\r\n"));
                    } else {
                        QFAIL("Unexpected request to local websocket test server");
                    }
                    socket->flush();
                    socket->disconnectFromHost();
                });
            }
        });

        Backend backend;
        WebSocketEventHandler events(backend);
        WebSocketConnector connector(events);
        QSignalSpy stateSpy(&connector,
                            &WebSocketConnector::connectionStateChanged);

        connector.open(baseUrl, QStringLiteral("test-token"));
        QTRY_VERIFY_WITH_TIMEOUT(socketAttempts >= 2, 12000);
        QCOMPARE(configRequests, 1);
        QVERIFY(connector.connectionState()
                != WebSocketConnector::ConnectionState::Disconnected);
        QVERIFY(!stateSpy.isEmpty());

        connector.close();
        QCOMPARE(connector.connectionState(),
                 WebSocketConnector::ConnectionState::Disconnected);
        const int beforeClose = socketAttempts;
        QTest::qWait(1800);
        QCOMPARE(socketAttempts, beforeClose);
    }
};

QTEST_MAIN(WebSocketReconnectTest)
#include "WebSocketReconnectTest.moc"
