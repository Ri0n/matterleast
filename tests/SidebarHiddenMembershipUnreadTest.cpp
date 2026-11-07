#include <QtTest>

#include <QJsonObject>
#include <QJsonDocument>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include "backend/NetworkRequest.h"

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/FollowingModel.h"
#include "channel-tree/ChannelQuickList.h"
#include "channel-tree/SidebarItem.h"

using namespace Mattermost;

namespace {

BackendChannel* makeChannel(Backend& backend)
{
    Storage& storage = backend.getStorage();
    storage.addUser(QJsonObject {
        {"id", "self"}, {"username", "self"}
    }, true);
    storage.addUser(QJsonObject {
        {"id", "other"}, {"username", "other"}
    });
    auto* team = storage.addTeam(QJsonObject {
        {"id", "team"}, {"name", "team"}, {"display_name", "Team"}
    });
    if (!team) return nullptr;
    return storage.addTeamChannel(*team, QJsonObject {
        {"id", "channel"}, {"team_id", "team"}, {"type", "O"},
        {"name", "engineering"}, {"display_name", "Engineering"},
        {"total_msg_count", 5}, {"total_msg_count_root", 5}
    });
}

void publish(Backend& backend, BackendChannel& channel, const QString& type,
             const QString& username, uint64_t timestamp, bool mentioned = false)
{
    const QJsonObject post {
        {"id", QStringLiteral("post-%1").arg(timestamp)},
        {"channel_id", channel.id},
        {"user_id", "other"},
        {"create_at", double(timestamp)},
        {"type", type},
        {"message", "event"},
        {"props", QJsonObject{{"username", username}}},
        {"_mmqt_current_user_mentioned", mentioned}
    };
    BackendPost eventPost(post, backend.getStorage());
    emit backend.onNewPost(channel, eventPost);
}


class ReadCheckpointHttpServer final : public QTcpServer
{
public:
    int checkpointRequests = 0;
    int viewRequests = 0;

    ReadCheckpointHttpServer()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                QTcpSocket* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected,
                        socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    QByteArray data = socket->property("requestData").toByteArray();
                    data += socket->readAll();
                    socket->setProperty("requestData", data);
                    if (!data.contains("\r\n\r\n")
                        || socket->property("answered").toBool()) {
                        return;
                    }
                    socket->setProperty("answered", true);
                    const QByteArray path = data.split(' ').value(1);
                    const bool checkpoint = path.contains("/set_unread");
                    if (checkpoint) {
                        ++checkpointRequests;
                    } else if (path.contains("/members/me/view")) {
                        ++viewRequests;
                    }
                    const QByteArray body = checkpoint
                        ? R"({"channel_id":"channel","last_viewed_at":999,"msg_count":3,"msg_count_root":3,"mention_count":0,"mention_count_root":0})"
                        : R"({"status":"OK"})";
                    const QByteArray response = QByteArray(
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Connection: close\r\nContent-Length: ")
                        + QByteArray::number(body.size())
                        + "\r\n\r\n" + body;
                    QTimer::singleShot(checkpoint ? 120 : 0, socket,
                        [socket, response] {
                        socket->write(response);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }
};

} // namespace

class SidebarHiddenMembershipUnreadTest final : public QObject
{
    Q_OBJECT
private slots:
    void invisibleMembershipEventDoesNotCreateUnread_data()
    {
        QTest::addColumn<QString>("type");
        QTest::newRow("join") << QStringLiteral("system_join_channel");
        QTest::newRow("leave") << QStringLiteral("system_leave_channel");
        QTest::newRow("add") << QStringLiteral("system_add_to_channel");
        QTest::newRow("remove") << QStringLiteral("system_remove_from_channel");
        QTest::newRow("join-team") << QStringLiteral("system_join_team");
        QTest::newRow("legacy") << QStringLiteral("system_join_leave");
    }

    void invisibleMembershipEventDoesNotCreateUnread()
    {
        QFETCH(QString, type);
        Backend backend;
        auto* channel = makeChannel(backend);
        QVERIFY(channel);
        auto& sidebar = SidebarService::instance(backend);

        // Even if the websocket mentions flag were set accidentally, an event
        // not visible in the timeline cannot create unread or mention state.
        publish(backend, *channel, type, QStringLiteral("other"), 2000, true);
        QVERIFY(!sidebar.isChannelUnread(*channel));
        QVERIFY(!sidebar.hasUnreadMention(channel->id));

        // A real post immediately afterwards must still create unread.
        publish(backend, *channel, QString(), QStringLiteral("other"), 3000);
        QVERIFY(sidebar.isChannelUnread(*channel));
    }

    void membershipEventAboutCurrentUserIsStillVisible()
    {
        Backend backend;
        auto* channel = makeChannel(backend);
        QVERIFY(channel);
        auto& sidebar = SidebarService::instance(backend);
        publish(backend, *channel, QStringLiteral("system_join_channel"),
                QStringLiteral("self"), 2000);
        QVERIFY(sidebar.isChannelUnread(*channel));
    }

    void readCheckpointFollowedByFullReadPreservesServerOrder()
    {
        ReadCheckpointHttpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(
            QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));

        Backend backend;
        BackendChannel* channel = makeChannel(backend);
        QVERIFY(channel);
        channel->last_post_at = 2000;
        auto& sidebar = SidebarService::instance(backend);
        QSignalSpy rewinds(&sidebar,
                           &SidebarService::channelUnreadRewound);

        bool checkpointFinished = false;
        bool checkpointSucceeded = false;
        sidebar.checkpointChannelRead(channel->id, QStringLiteral("post-next"),
            [&](bool success) {
            checkpointFinished = true;
            checkpointSucceeded = success;
        });
        QTRY_COMPARE(server.checkpointRequests, 1);

        // A newer full read takes priority, including when the originating
        // viewport has disappeared before the partial request completes.
        sidebar.markChannelViewedLocally(*channel);
        backend.markChannelAsViewed(*channel);
        QTRY_COMPARE(server.viewRequests, 1);

        // The server may deliver post_unread before the REST response. An
        // echo of our own partial checkpoint is not a manual unread action.
        sidebar.applyServerPostUnread(channel->id, QJsonObject {
            {"channel_id", channel->id},
            {"post_id", "post-next"},
            {"last_viewed_at", 999.0},
            {"msg_count", 3},
            {"msg_count_root", 3},
            {"mention_count", 0},
            {"mention_count_root", 0}
        });
        QCOMPARE(rewinds.count(), 0);

        QTRY_VERIFY(checkpointFinished);
        QVERIFY(checkpointSucceeded);
        QTRY_COMPARE(server.viewRequests, 2);
        QCOMPARE(sidebar.channelLastViewedTime(channel->id), uint64_t(2000));
    }

    void mentionedChannelRemainsInUnreadProjectionWhileUnread()
    {
        Backend backend;
        BackendChannel* channel = makeChannel(backend);
        QVERIFY(channel);

        auto& sidebar = SidebarService::instance(backend);
        SidebarCategory category;
        category.id = QStringLiteral("joined");
        category.teamId = QStringLiteral("team");
        category.type = QStringLiteral("channels");
        category.channelIds << channel->id;
        SidebarTeamState state;
        state.categories.insert(category.id, category);
        state.order << category.id;
        sidebar.applyLocalTeamState(category.teamId, state);

        ChannelQuickList unread;
        unread.initialize(backend);
        auto countRows = [&unread, channel](bool thread) {
            int count = 0;
            for (int i = 0; i < unread.topLevelItemCount(); ++i) {
                const auto* item = unread.topLevelItem(i);
                if (item->data(0, SidebarItem::ChannelIdRole).toString() == channel->id
                    && (item->data(0, SidebarItem::KindRole).toInt()
                        == (thread ? SidebarItem::Thread : SidebarItem::Channel))) {
                    ++count;
                }
            }
            return count;
        };

        QCOMPARE(countRows(false), 0);
        // A normal unread message makes the Channels row bold and adds the
        // corresponding Unread row via the same SidebarService authority.
        publish(backend, *channel, QString(), QStringLiteral("other"), 1000);
        QVERIFY(sidebar.isChannelTracked(channel->id));
        QVERIFY(sidebar.isChannelUnread(*channel));
        QCOMPARE(countRows(false), 1);

        // A mention creates a separate synthetic Following post, keyed by
        // t:<postId>, not a c:<channelId> Following conversation. It must not
        // suppress the still-unread channel's own row.
        publish(backend, *channel, QString(), QStringLiteral("other"), 2000, true);
        QVERIFY(sidebar.hasUnreadMention(channel->id));
        QVERIFY(sidebar.isChannelUnread(*channel));
        QCOMPARE(countRows(false), 1);
        QCOMPARE(countRows(true), 1);

        // Clicking/acknowledging that particular mention removes its
        // Following post, not the unrelated unread channel conversation.
        FollowingModel::instance(backend).acknowledgeVisibleSyntheticMention(
            channel->id, QStringLiteral("post-2000"));
        QCOMPARE(countRows(true), 0);
        QCOMPARE(countRows(false), 1);
        QVERIFY(sidebar.isChannelUnread(*channel));

        // Showing just Channels must not require an active Following entry.
        UnreadViewFilters filters;
        filters.following = false;
        unread.setUnreadFilters(filters);
        QCOMPARE(countRows(false), 1);
        QCOMPARE(countRows(true), 0);

        filters.channels = false;
        unread.setUnreadFilters(filters);
        QCOMPARE(countRows(false), 0);
        filters.channels = true;
        unread.setUnreadFilters(filters);
        QCOMPARE(countRows(false), 1);
    }

    void meaningfulSystemChangeIsNotSuppressed()
    {
        Backend backend;
        auto* channel = makeChannel(backend);
        QVERIFY(channel);
        auto& sidebar = SidebarService::instance(backend);
        publish(backend, *channel, QStringLiteral("system_header_change"),
                QStringLiteral("other"), 2000);
        QVERIFY(sidebar.isChannelUnread(*channel));
    }
};

QTEST_MAIN(SidebarHiddenMembershipUnreadTest)
#include "SidebarHiddenMembershipUnreadTest.moc"
