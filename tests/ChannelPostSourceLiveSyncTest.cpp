#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>

#include "backend/NetworkRequest.h"

#include "backend/Backend.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "chat-area/ChannelPostSource.h"

using namespace Mattermost;

namespace {

QString postId(int index)
{
    return QStringLiteral("post%1").arg(index, 22, 10, QLatin1Char('0'));
}

qint64 timestamp(int index)
{
    return 1000000000000LL + index * 1000LL;
}

QJsonObject rootPost(int index)
{
    return {
        {QStringLiteral("id"), postId(index)},
        {QStringLiteral("channel_id"), QStringLiteral("channel")},
        {QStringLiteral("user_id"), QStringLiteral("user")},
        {QStringLiteral("message"), QString::number(index)},
        {QStringLiteral("create_at"), timestamp(index)},
        {QStringLiteral("update_at"), timestamp(index)},
    };
}

QJsonObject channelJson(int rootCount, qint64 lastRootAt)
{
    return {
        {QStringLiteral("id"), QStringLiteral("channel")},
        {QStringLiteral("type"), QStringLiteral("D")},
        {QStringLiteral("name"), QStringLiteral("other-user")},
        {QStringLiteral("total_msg_count"), rootCount},
        {QStringLiteral("total_msg_count_root"), rootCount},
        {QStringLiteral("last_post_at"), lastRootAt},
        {QStringLiteral("last_root_post_at"), lastRootAt},
    };
}

// Real localhost HTTP cursor endpoint. The test deliberately keeps the
// channel's original root-count estimate stale after the server grows.
class ChannelServer final : public QTcpServer
{
public:
    int latest = 1;
    QList<QUrl> requests;

    ChannelServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket,
                        [this, socket] {
                    QByteArray bytes = socket->property("incoming").toByteArray()
                        + socket->readAll();
                    socket->setProperty("incoming", bytes);
                    if (!bytes.contains("\r\n\r\n")
                        || socket->property("answered").toBool()) return;
                    socket->setProperty("answered", true);

                    const QUrl url(QString::fromUtf8(bytes.split(' ').value(1)));
                    requests.push_back(url);
                    const QUrlQuery query(url);
                    int first = 1;
                    if (query.hasQueryItem(QStringLiteral("after"))) {
                        const QString cursor =
                            query.queryItemValue(QStringLiteral("after"));
                        for (int i = 1; i <= latest; ++i) {
                            if (postId(i) == cursor) {
                                first = i + 1;
                                break;
                            }
                        }
                    } else {
                        first = qMax(1, latest - 9);
                    }
                    QJsonObject posts;
                    QJsonArray order;
                    const int limit = query.queryItemValue(
                        QStringLiteral("per_page")).toInt();
                    int count = 0;
                    for (int i = latest; i >= first && count < limit; --i) {
                        order.append(postId(i));
                        posts.insert(postId(i), rootPost(i));
                        ++count;
                    }
                    const QByteArray json = QJsonDocument(QJsonObject {
                        {QStringLiteral("order"), order},
                        {QStringLiteral("posts"), posts}
                    }).toJson(QJsonDocument::Compact);
                    const QByteArray reply = QByteArrayLiteral(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Connection: close\r\nContent-Length: ")
                        + QByteArray::number(json.size()) + "\r\n\r\n" + json;
                    socket->write(reply);
                    socket->disconnectFromHost();
                });
            }
        });
    }
};

} // namespace

class ChannelPostSourceLiveSyncTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void recoveredConnectionFetchesMissedRootsInOrder()
    {
        ChannelServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(
            QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        BackendChannel channel(
            backend.getStorage(), channelJson(1, timestamp(1)));
        QVERIFY(channel.addPost(rootPost(1)));
        ChannelPostSource source(backend, channel);
        QCOMPARE(source.itemCount(), 1);
        // Let initial cache-tail validation finish against the old server
        // snapshot before simulating messages that arrived during sleep.
        QTRY_VERIFY(!server.requests.isEmpty());
        QTest::qWait(30);
        server.requests.clear();

        server.latest = 4;
        emit backend.onRealtimeRecovered();
        QTRY_COMPARE(source.itemCount(), 4);
        for (int i = 1; i <= 4; ++i) {
            QCOMPARE(source.postIdAt(i - 1), postId(i));
            QVERIFY(source.isAvailable(i - 1));
        }
        QCOMPARE(source.indexOfPost(postId(4)), 3);
        bool fetchedSinceFirst = false;
        for (const QUrl& request : server.requests) {
            if (QUrlQuery(request).queryItemValue(QStringLiteral("after"))
                == postId(1)) {
                fetchedSinceFirst = true;
            }
        }
        QVERIFY(fetchedSinceFirst);
    }

    void transientBackendEventExtendsSemanticTail()
    {
        Backend backend;
        BackendChannel channel(
            backend.getStorage(), channelJson(1, timestamp(1)));
        BackendPost* first = channel.addPost(rootPost(1));
        QVERIFY(first);

        ChannelPostSource source(backend, channel);
        QCOMPARE(source.itemCount(), 1);
        QCOMPARE(source.postIdAt(0), postId(1));
        QVERIFY(source.isAvailable(0));

        // Reproduce WebSocketEventHandler's cold-channel path: the post is
        // delivered globally but deliberately not retained in BackendChannel.
        BackendPost transient(rootPost(2), backend.getStorage());
        QVERIFY(!channel.postIdToPost.contains(transient.id));

        QSignalSpy countChanged(&source, &AbstractPostSource::itemCountChanged);
        emit backend.onNewPost(channel, transient);

        QCOMPARE(countChanged.size(), 1);
        QCOMPARE(source.itemCount(), 2);
        QCOMPARE(source.indexOfPost(postId(2)), 1);
        QCOMPARE(source.postIdAt(1), postId(2));
        QVERIFY(!source.isAvailable(1));

        // A later normal server page rehydrates the body. The semantic row
        // already exists, so it becomes available in place rather than appearing
        // only after source/process reconstruction.
        QSignalSpy bodyChanged(
            &source, &AbstractPostSource::bodyAvailabilityChanged);
        channel.mergePostContext(
            QJsonArray {postId(2)},
            QJsonObject {{postId(2), rootPost(2)}});

        QVERIFY(source.isAvailable(1));
        QVERIFY(source.postAt(1));
        QCOMPARE(source.postAt(1)->id, postId(2));
        QVERIFY(!bodyChanged.isEmpty());
    }

    void residentBackendEventIsNotAppendedTwice()
    {
        Backend backend;
        BackendChannel channel(
            backend.getStorage(), channelJson(1, timestamp(1)));
        QVERIFY(channel.addPost(rootPost(1)));

        ChannelPostSource source(backend, channel);
        QCOMPARE(source.itemCount(), 1);

        BackendPost* second = channel.addPost(rootPost(2));
        QVERIFY(second);

        emit channel.onNewPost(*second);
        QCOMPARE(source.itemCount(), 2);
        QCOMPARE(source.indexOfPost(second->id), 1);

        // WebSocketEventHandler emits the global notification after the
        // channel-local one for resident posts.
        emit backend.onNewPost(channel, *second);
        QCOMPARE(source.itemCount(), 2);
        QCOMPARE(source.indexOfPost(second->id), 1);
    }
};

QTEST_MAIN(ChannelPostSourceLiveSyncTest)

#include "ChannelPostSourceLiveSyncTest.moc"
