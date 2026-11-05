#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "chat-area/ChannelPostSource.h"
#include "chat-area/FilteredPostSource.h"
#include "chat-area/PostFilterPolicy.h"

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

QJsonObject rootPost(int index, int ordinaryCount)
{
    const bool hidden = index >= ordinaryCount;
    return {
        {QStringLiteral("id"), postId(index)},
        {QStringLiteral("channel_id"), QStringLiteral("channel")},
        {QStringLiteral("user_id"), QStringLiteral("other")},
        {QStringLiteral("message"),
         hidden ? QStringLiteral("other joined the channel.")
                : QStringLiteral("real message %1").arg(index)},
        {QStringLiteral("type"),
         hidden ? QStringLiteral("system_join_channel") : QString()},
        {QStringLiteral("props"), QJsonObject {
            {QStringLiteral("username"), QStringLiteral("other")}
        }},
        {QStringLiteral("create_at"), double(timestamp(index))},
        {QStringLiteral("update_at"), double(timestamp(index))}
    };
}

class HiddenHistoryServer final : public QTcpServer
{
public:
    int ordinaryCount = 19;
    int hiddenCount = 16;
    int requests = 0;

    HiddenHistoryServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    if (socket->property("answered").toBool()) {
                        return;
                    }
                    const QByteArray received =
                        socket->property("incoming").toByteArray() + socket->readAll();
                    socket->setProperty("incoming", received);
                    if (!received.contains("\r\n\r\n")) {
                        return;
                    }
                    socket->setProperty("answered", true);
                    ++requests;

                    const QUrl url(QString::fromUtf8(received.split(' ').value(1)));
                    const QUrlQuery query(url);
                    const int page = qMax(0, query.queryItemValue(
                        QStringLiteral("page")).toInt());
                    const int perPage = qMax(1, query.queryItemValue(
                        QStringLiteral("per_page")).toInt());
                    const int total = ordinaryCount + hiddenCount;
                    const int offset = page * perPage;

                    QJsonObject posts;
                    QJsonArray order;
                    const int end = qMin(total, offset + perPage);
                    for (int cursor = offset; cursor < end; ++cursor) {
                        const int index = total - 1 - cursor;
                        order.append(postId(index));
                        posts.insert(postId(index), rootPost(index, ordinaryCount));
                    }
                    const QJsonObject payload {
                        {QStringLiteral("order"), order},
                        {QStringLiteral("posts"), posts},
                        {QStringLiteral("prev_post_id"),
                         end < total ? postId(total - 1 - end) : QString()},
                        {QStringLiteral("next_post_id"),
                         offset > 0 ? postId(total - offset) : QString()}
                    };
                    const QByteArray body =
                        QJsonDocument(payload).toJson(QJsonDocument::Compact);
                    const QByteArray response =
                        QByteArrayLiteral("HTTP/1.1 200 OK\r\n")
                        + QByteArrayLiteral("Content-Type: application/json\r\n")
                        + QByteArrayLiteral("Connection: close\r\n")
                        + QByteArrayLiteral("Content-Length: ")
                        + QByteArray::number(body.size())
                        + QByteArrayLiteral("\r\n\r\n") + body;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    bool start()
    {
        if (!listen(QHostAddress::LocalHost)) {
            return false;
        }
        NetworkRequest::setHost(
            QStringLiteral("http://127.0.0.1:%1/").arg(serverPort()));
        return true;
    }
};

QJsonObject channelJson(int ordinaryCount, int hiddenCount)
{
    return {
        {QStringLiteral("id"), QStringLiteral("channel")},
        {QStringLiteral("name"), QStringLiteral("engineering")},
        {QStringLiteral("display_name"), QStringLiteral("Engineering")},
        {QStringLiteral("type"), QStringLiteral("O")},
        // Mattermost excludes routine membership posts from these counters.
        {QStringLiteral("total_msg_count"), ordinaryCount},
        {QStringLiteral("total_msg_count_root"), ordinaryCount},
        {QStringLiteral("last_post_at"),
         double(timestamp(ordinaryCount + hiddenCount - 1))},
        {QStringLiteral("last_root_post_at"),
         double(timestamp(ordinaryCount + hiddenCount - 1))}
    };
}

} // namespace

class ChannelPostSourceHiddenHistoryTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void discoversNormalMessagesBeforeBuriedMembershipEvents_data()
    {
        QTest::addColumn<int>("ordinaryCount");
        QTest::addColumn<int>("hiddenCount");

        // Reproduces 19 estimated rows collapsing to only 3 visible ones.
        QTest::newRow("three-visible") << 19 << 16;
        // With all 19 estimated slots hidden the filtered view has zero rows,
        // so it cannot request older history by scrolling. The source itself
        // must still discover the older channel messages.
        QTest::newRow("all-hidden") << 19 << 19;
    }

    void discoversNormalMessagesBeforeBuriedMembershipEvents()
    {
        QFETCH(int, ordinaryCount);
        QFETCH(int, hiddenCount);

        HiddenHistoryServer server;
        server.ordinaryCount = ordinaryCount;
        server.hiddenCount = hiddenCount;
        QVERIFY(server.start());

        Backend backend;
        backend.getStorage().addUser(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("self")},
            {QStringLiteral("username"), QStringLiteral("self")}
        }, true);
        BackendChannel channel(backend.getStorage(),
                               channelJson(ordinaryCount, hiddenCount));
        ChannelPostSource raw(backend, channel);
        FilteredPostSource visible(raw, [](const BackendPost& post) {
            return shouldShowMainTimelinePost(
                post.type, post.props.toObject(), QStringLiteral("self"));
        });
        QCOMPARE(raw.itemCount(), ordinaryCount);
        QCOMPARE(visible.itemCount(), ordinaryCount);

        // The first page spans the entire inaccurate root-message estimate.
        // It is mostly or entirely hidden routine membership events.
        visible.requestRange(0, ordinaryCount - 1,
                             AbstractPostSource::RequestReason::Initial, 0);

        // The estimated oldest row was concrete, but NOT the actual oldest
        // server post. A bounded boundary probe must grow the raw source and
        // recover older normal messages without a second request by the view.
        QTRY_VERIFY_WITH_TIMEOUT(raw.itemCount() > ordinaryCount, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(
            visible.indexOfPost(postId(0)) >= 0, 15000);
        QVERIFY(visible.itemCount() >= ordinaryCount);
        QVERIFY(visible.postAt(0));
        QCOMPARE(visible.postAt(0)->id, postId(0));
        QVERIFY(server.requests >= 2);
    }
};

QTEST_MAIN(ChannelPostSourceHiddenHistoryTest)
#include "ChannelPostSourceHiddenHistoryTest.moc"
