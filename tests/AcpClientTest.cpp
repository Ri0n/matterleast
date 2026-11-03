#include <QtTest>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTimer>

#include "ai/AcpClient.h"

using namespace Mattermost;

class AcpClientTest final : public QObject
{
    Q_OBJECT
private:
    static AcpClient::Configuration configuration()
    {
        AcpClient::Configuration config;
        config.program = QString::fromUtf8(ACP_TEST_AGENT_PATH);
        config.workingDirectory = QDir::tempPath();
        config.mcpProgram = QCoreApplication::applicationFilePath();
        config.mcpEndpoint = QStringLiteral("local-endpoint");
        config.mcpToken = QStringLiteral("secret");
        return config;
    }

private slots:
    void initializeAndStreamMultipleTurns()
    {
        AcpClient client;
        QSignalSpy ready(&client, &AcpClient::readyChanged);
        QSignalSpy finished(&client, &AcpClient::turnFinished);
        QString output;
        int thoughts = 0, tools = 0;
        connect(&client, &AcpClient::messageChunk, this,
                [&](const QString& text) { output += text; });
        connect(&client, &AcpClient::thoughtChunk, this,
                [&](const QString&) { ++thoughts; });
        connect(&client, &AcpClient::toolChanged, this,
                [&](const QString&, const QString&) { ++tools; });

        client.start(configuration());
        QTRY_VERIFY(client.ready());
        QCOMPARE(client.sessionId(), QStringLiteral("test-session"));
        QVERIFY(!ready.isEmpty());
        QVERIFY(client.prompt(QStringLiteral("first turn")));
        QVERIFY(client.busy());
        QVERIFY(!client.prompt(QStringLiteral("concurrent prompt")));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.at(0).at(0).toString(), QStringLiteral("end_turn"));
        QCOMPARE(output, QStringLiteral("hello there"));
        QVERIFY(thoughts > 0);
        QVERIFY(tools > 0);
        QVERIFY(!client.busy());

        output.clear();
        QVERIFY(client.prompt(QStringLiteral("second turn")));
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(output, QStringLiteral("hello there"));
    }

    void permissionsAreExplicitAndStructured()
    {
        AcpClient client;
        QSignalSpy finished(&client, &AcpClient::turnFinished);
        QString output;
        int requests = 0;
        connect(&client, &AcpClient::messageChunk, this,
                [&](const QString& text) { output += text; });
        connect(&client, &AcpClient::permissionRequested, this,
                [&](const QJsonValue& id, const QJsonObject& params) {
            ++requests;
            QCOMPARE(params.value(QStringLiteral("sessionId")).toString(),
                     QStringLiteral("test-session"));
            QCOMPARE(params.value(QStringLiteral("options")).toArray().size(), 2);
            QTimer::singleShot(0, &client, [&client, id] {
                client.answerPermission(id, QStringLiteral("allow"));
            });
        });
        client.start(configuration());
        QTRY_VERIFY(client.ready());
        QVERIFY(client.prompt(QStringLiteral("permission")));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(requests, 1);
        QCOMPARE(output, QStringLiteral("approved"));
    }

    void cancelAndDenyUndeclaredClientMethods()
    {
        AcpClient client;
        QSignalSpy finished(&client, &AcpClient::turnFinished);
        QString output;
        connect(&client, &AcpClient::messageChunk, this,
                [&](const QString& text) { output += text; });
        client.start(configuration());
        QTRY_VERIFY(client.ready());

        QVERIFY(client.prompt(QStringLiteral("unsupported")));
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(output, QStringLiteral("unsupported-method-denied"));

        QVERIFY(client.prompt(QStringLiteral("cancel")));
        client.cancel();
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(finished.at(1).at(0).toString(), QStringLiteral("cancelled"));
        QVERIFY(!client.busy());
    }

    void resumesWithTheSameMcpConfiguration()
    {
        AcpClient client;
        QSignalSpy failed(&client, &AcpClient::failed);
        client.start(configuration());
        QTRY_VERIFY(client.ready());
        const QString id = client.sessionId();
        QVERIFY(!id.isEmpty());
        client.stop();
        client.start(configuration(), id);
        QTRY_VERIFY(client.ready());
        QCOMPARE(client.sessionId(), id);
        QCOMPARE(failed.size(), 0);
    }

    void refusesUnusableLaunchConfiguration()
    {
        AcpClient client;
        QSignalSpy failed(&client, &AcpClient::failed);
        auto config = configuration();
        config.mcpToken.clear();
        client.start(config);
        QCOMPARE(failed.size(), 1);
        QVERIFY(!client.ready());
        QVERIFY(!client.prompt(QStringLiteral("never")));
    }
};

QTEST_GUILESS_MAIN(AcpClientTest)
#include "AcpClientTest.moc"
