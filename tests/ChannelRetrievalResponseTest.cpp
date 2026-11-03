#include <QtTest>

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSharedPointer>
#include <QTcpServer>
#include <QTcpSocket>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"

using namespace Mattermost;

class ChannelRetrievalResponseTest final : public QObject
{
    Q_OBJECT

private slots:
    void missingAndInvalidTypesAreUnknown()
    {
        const auto type = [](const QString& value) {
            return BackendChannel::getChannelType(QJsonObject{{"type", value}});
        };

        QCOMPARE(BackendChannel::getChannelType(QJsonObject{}),
                 static_cast<uint32_t>(BackendChannel::unknown));
        QCOMPARE(type(QString()), static_cast<uint32_t>(BackendChannel::unknown));
        QCOMPARE(type(QStringLiteral("DX")), static_cast<uint32_t>(BackendChannel::unknown));
        QCOMPARE(type(QStringLiteral("D")), static_cast<uint32_t>(BackendChannel::directChannel));
        QCOMPARE(type(QStringLiteral("G")), static_cast<uint32_t>(BackendChannel::groupChannel));
        QCOMPARE(type(QStringLiteral("O")), static_cast<uint32_t>(BackendChannel::publicChannel));
        QCOMPARE(type(QStringLiteral("P")), static_cast<uint32_t>(BackendChannel::privateChannel));
    }

    void storageRejectsErrorBodies()
    {
        Storage storage;
        const QJsonObject serverError {
            {"id", "api.context.permissions.app_error"},
            {"message", "You do not have permission"},
            {"status_code", 403}
        };

        QVERIFY(!storage.addDirectChannel(serverError));
        QVERIFY(!storage.getChannelById(QStringLiteral("api.context.permissions.app_error")));
        QVERIFY(storage.channels.isEmpty());

        const QJsonObject malformedDirect {
            {"id", "dm-1"}, {"type", "D"}, {"name", ""}
        };
        QVERIFY(!storage.addDirectChannel(malformedDirect));
        QVERIFY(!storage.addDirectChannel(QJsonObject{
            {"id", "dm-1"}, {"type", "G"}, {"name", "user-1__user-2"}
        }));

        BackendTeam* team = storage.addTeam(QJsonObject{
            {"id", "team-1"}, {"name", "team"}
        });
        QVERIFY(team);
        QVERIFY(!storage.addTeamChannel(*team, serverError));
        QVERIFY(!storage.addChannel(*team, serverError));
        QVERIFY(storage.channels.isEmpty());
        QVERIFY(team->channels.empty());
    }

    void httpErrorAndMalformedSuccessAreIgnored()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/")
                                    .arg(server.serverPort()));

        int channelRequests = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (QTcpSocket* socket = server.nextPendingConnection()) {
                const auto buffer = QSharedPointer<QByteArray>::create();
                connect(socket, &QTcpSocket::readyRead, socket,
                        [&, socket, buffer] {
                    buffer->append(socket->readAll());
                    const int end = buffer->indexOf("\r\n\r\n");
                    if (end < 0) return;
                    const QByteArray firstLine = buffer->left(buffer->indexOf("\r\n"));
                    const bool isChannel = firstLine.contains("/channels/");
                    if (isChannel) ++channelRequests;

                    int status = 200;
                    QJsonObject payload;
                    if (firstLine.contains("/channels/denied")) {
                        status = 403;
                        payload = QJsonObject {
                            {"id", "api.context.permissions.app_error"},
                            {"message", "You do not have permission"},
                            {"status_code", 403}
                        };
                    } else if (firstLine.contains("/channels/wrong-type")) {
                        payload = QJsonObject {
                            {"id", "wrong-type"}, {"type", "G"},
                            {"name", "user-1__user-2"}
                        };
                    } else if (firstLine.contains("/channels/wrong-id")) {
                        payload = QJsonObject {
                            {"id", "another-id"}, {"type", "D"},
                            {"name", "user-1__user-2"}
                        };
                    } else if (firstLine.contains("/channels/valid")) {
                        payload = QJsonObject {
                            {"id", "valid"}, {"type", "D"},
                            {"name", "user-1__user-2"}
                        };
                    }
                    const QByteArray body =
                        QJsonDocument(payload).toJson(QJsonDocument::Compact);
                    socket->write(
                        QByteArray("HTTP/1.1 ") + QByteArray::number(status)
                        + (status == 200 ? " OK\r\n" : " Forbidden\r\n")
                        + "Content-Type: application/json\r\n"
                          "Connection: close\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->flush();
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QTcpSocket::deleteLater);
            }
        });

        Backend backend;
        Storage& storage = backend.getStorage();
        storage.addUser(QJsonObject{
            {"id", "user-1"}, {"username", "user-1"}
        }, true);

        backend.retrieveDirectChannel(QStringLiteral("denied"));
        QTRY_COMPARE(channelRequests, 1);
        QTest::qWait(50);
        QVERIFY(storage.channels.isEmpty());

        backend.retrieveDirectChannel(QStringLiteral("wrong-type"));
        QTRY_COMPARE(channelRequests, 2);
        QTest::qWait(50);
        QVERIFY(storage.channels.isEmpty());

        backend.retrieveDirectChannel(QStringLiteral("wrong-id"));
        QTRY_COMPARE(channelRequests, 3);
        QTest::qWait(50);
        QVERIFY(storage.channels.isEmpty());

        backend.retrieveDirectChannel(QStringLiteral("valid"));
        QTRY_VERIFY(storage.getChannelById(QStringLiteral("valid")) != nullptr);
        QCOMPARE(storage.getChannelById(QStringLiteral("valid"))->type,
                 static_cast<uint32_t>(BackendChannel::directChannel));
        QVERIFY(!storage.getChannelById(QStringLiteral("api.context.permissions.app_error")));
        QVERIFY(!storage.getChannelById(QStringLiteral("another-id")));
    }
};

QTEST_MAIN(ChannelRetrievalResponseTest)
#include "ChannelRetrievalResponseTest.moc"
