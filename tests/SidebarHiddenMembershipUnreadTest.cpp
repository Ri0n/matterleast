#include <QtTest>

#include <QJsonObject>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

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
