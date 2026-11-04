#include <QtTest>

#include <QDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>

#include "backend/Backend.h"
#include "backend/ChannelTopicPolicy.h"
#include "backend/NetworkRequest.h"
#include "backend/types/BackendChannel.h"
#include "backend/Storage.h"
#include "chat-area/ChannelTopicEditDialog.h"

using namespace Mattermost;

namespace {
class FakeChannelServer : public QTcpServer
{
public:
    int httpStatus = 200;
    QString returnedHeader;
    QByteArray requestMethod;
    QByteArray requestPath;
    QJsonObject requestBody;
    int requests = 0;

    FakeChannelServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->property("responded").toBool()) return;
                    const QByteArray data = socket->property("received").toByteArray()
                        + socket->readAll();
                    socket->setProperty("received", data);
                    const int headerEnd = data.indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    const QByteArray headers = data.left(headerEnd);
                    int contentLength = 0;
                    for (const QByteArray& line : headers.split('\n')) {
                        if (line.trimmed().toLower().startsWith("content-length:")) {
                            contentLength = line.mid(line.indexOf(':') + 1)
                                                .trimmed().toInt();
                        }
                    }
                    if (data.size() - headerEnd - 4 < contentLength) return;
                    socket->setProperty("responded", true);
                    ++requests;
                    const auto requestLine = headers.split('\n').first().trimmed()
                        .split(' ');
                    requestMethod = requestLine.value(0);
                    requestPath = requestLine.value(1);
                    requestBody = QJsonDocument::fromJson(
                        data.mid(headerEnd + 4, contentLength)).object();

                    const bool allowed = httpStatus == 200;
                    const QJsonObject response = allowed
                        ? QJsonObject {
                            {"id", "test-channel"},
                            {"type", "O"},
                            {"header", returnedHeader},
                            {"name", "engineering"},
                            {"display_name", "Engineering"},
                            {"purpose", "Planning"}
                        }
                        : QJsonObject{{"message", "Permission denied"}};
                    const QByteArray body =
                        QJsonDocument(response).toJson(QJsonDocument::Compact);
                    const QByteArray wire =
                        QByteArrayLiteral("HTTP/1.1 ")
                        + (allowed ? QByteArrayLiteral("200 OK")
                                   : QByteArrayLiteral("403 Forbidden"))
                        + QByteArrayLiteral("\r\nContent-Type: application/json\r\n")
                        + QByteArrayLiteral("Connection: close\r\nContent-Length: ")
                        + QByteArray::number(body.size())
                        + QByteArrayLiteral("\r\n\r\n") + body;
                    socket->write(wire);
                    socket->disconnectFromHost();
                });
            }
        });
    }
};

BackendChannel* makeChannel(Backend& backend)
{
    auto& storage = backend.getStorage();
    storage.addUser(QJsonObject {
        {"id", "user"}, {"username", "tester"}
    }, true);
    auto* team = storage.addTeam(QJsonObject {
        {"id", "team"}, {"name", "team"}, {"display_name", "Team"}
    });
    if (!team) return nullptr;
    return storage.addTeamChannel(*team, QJsonObject {
        {"id", "test-channel"}, {"team_id", "team"}, {"name", "engineering"},
        {"type", "O"}, {"display_name", "Engineering"},
        {"purpose", "Planning"}, {"header", "Initial topic"}
    });
}

bool prepare(FakeChannelServer& server)
{
    if (!server.listen(QHostAddress::LocalHost)) return false;
    NetworkRequest::clearToken();
    NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/")
                             .arg(server.serverPort()));
    return true;
}
} // namespace

class ChannelTopicTest final : public QObject
{
    Q_OBJECT
private slots:
    void patchOnlyHeaderAndUpdateCanonicalChannel()
    {
        FakeChannelServer server;
        QVERIFY(prepare(server));
        server.returnedHeader = QStringLiteral("New **Markdown** topic");
        Backend backend;
        auto* channel = makeChannel(backend);
        QVERIFY(channel);
        QSignalSpy changed(channel, &BackendChannel::onUpdated);

        int responses = 0;
        bool ok = false;
        backend.editChannelHeader(*channel, server.returnedHeader,
            [&](bool success, const QString&) {
                ++responses;
                ok = success;
            });
        QTRY_COMPARE(responses, 1);
        QVERIFY(ok);
        QCOMPARE(server.requests, 1);
        QCOMPARE(server.requestMethod, QByteArrayLiteral("PUT"));
        QVERIFY(server.requestPath.endsWith(
            QByteArrayLiteral("/channels/test-channel/patch")));
        QCOMPARE(server.requestBody.size(), 1);
        QCOMPARE(server.requestBody.value(QStringLiteral("header")).toString(),
                 server.returnedHeader);
        QCOMPARE(channel->header, server.returnedHeader);
        QCOMPARE(channel->purpose, QStringLiteral("Planning"));
        QCOMPARE(channel->display_name, QStringLiteral("Engineering"));
        QCOMPARE(changed.count(), 1);
    }

    void forbiddenLeavesLocalStateUnchanged()
    {
        FakeChannelServer server;
        QVERIFY(prepare(server));
        server.httpStatus = 403;
        Backend backend;
        auto* channel = makeChannel(backend);
        QVERIFY(channel);
        QSignalSpy changed(channel, &BackendChannel::onUpdated);
        int responses = 0;
        QString error;
        backend.editChannelHeader(*channel, QStringLiteral("Forbidden change"),
            [&](bool success, const QString& description) {
                QVERIFY(!success);
                ++responses;
                error = description;
            });
        QTRY_COMPARE(responses, 1);
        QCOMPARE(server.requests, 1);
        QVERIFY(error.contains(QStringLiteral("permission"), Qt::CaseInsensitive));
        QCOMPARE(channel->header, QStringLiteral("Initial topic"));
        QCOMPARE(changed.count(), 0);
    }

    void rejectsInvalidLengthWithoutNetwork()
    {
        FakeChannelServer server;
        QVERIFY(prepare(server));
        Backend backend;
        auto* channel = makeChannel(backend);
        QVERIFY(channel);
        int responses = 0;
        QString error;
        backend.editChannelHeader(*channel,
            QString(ChannelTopicPolicy::MaximumLength + 1, QLatin1Char('a')),
            [&](bool success, const QString& description) {
                QVERIFY(!success);
                ++responses;
                error = description;
            });
        QCOMPARE(responses, 1);
        QVERIFY(!error.isEmpty());
        QCOMPARE(server.requests, 0);
        QCOMPARE(channel->header, QStringLiteral("Initial topic"));
    }

    void editorValidatesLengthAndKeepsFailedEdits()
    {
        FakeChannelServer server;
        QVERIFY(prepare(server));
        server.httpStatus = 403;
        Backend backend;
        auto* channel = makeChannel(backend);
        QVERIFY(channel);
        ChannelTopicEditDialog dialog(backend, *channel);
        auto* editor = dialog.findChild<QPlainTextEdit*>(
            QStringLiteral("channelTopicEditor"));
        auto* save = dialog.findChild<QPushButton*>(
            QStringLiteral("channelTopicSave"));
        auto* error = dialog.findChild<QLabel*>(
            QStringLiteral("channelTopicError"));
        QVERIFY(editor);
        QVERIFY(save);
        QVERIFY(error);
        QVERIFY(!save->isEnabled());
        editor->setPlainText(
            QString(ChannelTopicPolicy::MaximumLength + 1, QLatin1Char('x')));
        QVERIFY(!save->isEnabled());

        const QString submitted = QStringLiteral("Keep **this** edit");
        editor->setPlainText(submitted);
        QVERIFY(save->isEnabled());
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_COMPARE(server.requests, 1);
        QTRY_VERIFY(!error->isHidden());
        QVERIFY(error->text().contains(
            QStringLiteral("permission"), Qt::CaseInsensitive));
        QCOMPARE(editor->toPlainText(), submitted);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(channel->header, QStringLiteral("Initial topic"));
    }

    void editorClosesOnSuccessfulPatch()
    {
        FakeChannelServer server;
        QVERIFY(prepare(server));
        server.returnedHeader = QStringLiteral("Updated topic");
        Backend backend;
        auto* channel = makeChannel(backend);
        QVERIFY(channel);
        ChannelTopicEditDialog dialog(backend, *channel);
        auto* editor = dialog.findChild<QPlainTextEdit*>(
            QStringLiteral("channelTopicEditor"));
        auto* save = dialog.findChild<QPushButton*>(
            QStringLiteral("channelTopicSave"));
        QVERIFY(editor);
        QVERIFY(save);
        editor->setPlainText(server.returnedHeader);
        QVERIFY(save->isEnabled());
        QTest::mouseClick(save, Qt::LeftButton);
        QTRY_COMPARE(dialog.result(), int(QDialog::Accepted));
        QCOMPARE(channel->header, server.returnedHeader);
    }
};

QTEST_MAIN(ChannelTopicTest)
#include "ChannelTopicTest.moc"
