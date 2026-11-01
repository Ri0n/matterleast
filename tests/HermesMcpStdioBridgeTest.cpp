#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QUuid>

class HermesMcpStdioBridgeTest final : public QObject
{
    Q_OBJECT
private slots:
    void missingCredentialsFailClosed()
    {
        QProcess bridge;
        auto env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("MATTERLEAST_MCP_SOCKET"));
        env.remove(QStringLiteral("MATTERLEAST_MCP_TOKEN"));
        bridge.setProcessEnvironment(env);
        bridge.start(QStringLiteral(MATTERLEAST_MCP_BRIDGE_EXECUTABLE), QStringList{});
        QVERIFY(bridge.waitForFinished(5000));
        QCOMPARE(bridge.exitCode(), 1);
        QVERIFY(bridge.readAllStandardOutput().isEmpty());
    }

    void forwardsJsonRpcAndDoesNotRespondToNotifications()
    {
        QLocalServer server;
        const QString endpoint = QStringLiteral("matterleast-mcp-test-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY2(server.listen(endpoint), qPrintable(server.errorString()));

        QProcess bridge;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("MATTERLEAST_MCP_SOCKET"), endpoint);
        env.insert(QStringLiteral("MATTERLEAST_MCP_TOKEN"),
                   QStringLiteral("test-capability"));
        bridge.setProcessEnvironment(env);
        QSignalSpy connections(&server, &QLocalServer::newConnection);
        bridge.start(QStringLiteral(MATTERLEAST_MCP_BRIDGE_EXECUTABLE), QStringList{});
        QVERIFY(bridge.waitForStarted(5000));
        if (!server.hasPendingConnections())
            QVERIFY(connections.wait(5000));
        QLocalSocket* peer = server.nextPendingConnection();
        QVERIFY(peer);

        QByteArray received;
        int count = 0;
        QObject::connect(peer, &QLocalSocket::readyRead, peer, [peer, &received, &count] {
            received += peer->readAll();
            while (true) {
                const int end = received.indexOf('\n');
                if (end < 0)
                    break;
                const auto document = QJsonDocument::fromJson(received.left(end));
                received.remove(0, end + 1);
                if (!document.isObject())
                    continue;
                const auto wrapper = document.object();
                if (wrapper.value(QStringLiteral("token")).toString()
                    != QStringLiteral("test-capability")) {
                    continue;
                }
                ++count;
                const auto rpc = wrapper.value(QStringLiteral("mcp")).toObject();
                if (rpc.contains(QStringLiteral("id"))) {
                    const QJsonObject reply {
                        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), rpc.value(QStringLiteral("id"))},
                        {QStringLiteral("result"), QJsonObject {}}
                    };
                    peer->write(QJsonDocument(reply).toJson(QJsonDocument::Compact) + '\n');
                }
            }
        });

        const QByteArray init = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})";
        const QByteArray notification = R"({"jsonrpc":"2.0","method":"notifications/initialized"})";
        bridge.write(init + '\n' + notification + '\n');
        bridge.closeWriteChannel();

        QSignalSpy output(&bridge, &QProcess::readyReadStandardOutput);
        if (!bridge.bytesAvailable())
            QVERIFY(output.wait(5000));
        const QByteArray response = bridge.readAllStandardOutput();
        const auto document = QJsonDocument::fromJson(response.trimmed());
        QVERIFY(document.isObject());
        QCOMPARE(document.object().value(QStringLiteral("id")).toInt(), 1);
        QTRY_COMPARE(count, 2);
        QVERIFY(bridge.waitForFinished(5000));
        QVERIFY(bridge.readAllStandardOutput().isEmpty());
    }
};

QTEST_GUILESS_MAIN(HermesMcpStdioBridgeTest)
#include "HermesMcpStdioBridgeTest.moc"
