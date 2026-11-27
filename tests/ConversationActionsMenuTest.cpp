#include <QtTest>

#include <QAction>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QMenu>

#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "ui/ConversationActionsMenu.h"
#include "navigation/AppNavigationService.h"

using namespace Mattermost;

namespace {

QAction* actionNamed(QMenu& menu, const QString& name)
{
    for (QAction* action : menu.actions()) {
        if (action->objectName() == name) {
            return action;
        }
    }
    return nullptr;
}

QStringList actionSignature(QMenu& menu)
{
    QStringList result;
    for (QAction* action : menu.actions()) {
        result << action->objectName() + QLatin1Char('|')
                      + action->text() + QLatin1Char('|')
                      + action->property("matterleastMenuIconPath").toString();
    }
    return result;
}

BackendChannel* addTeamChannel(Backend& backend, const QString& id)
{
    Storage& storage = backend.getStorage();
    BackendTeam* team = storage.addTeam(QJsonObject {
        {QStringLiteral("id"), QStringLiteral("team")},
        {QStringLiteral("name"), QStringLiteral("team")},
        {QStringLiteral("display_name"), QStringLiteral("Team")},
    });
    if (!team) {
        return nullptr;
    }
    return storage.addTeamChannel(*team, QJsonObject {
        {QStringLiteral("id"), id},
        {QStringLiteral("team_id"), QStringLiteral("team")},
        {QStringLiteral("type"), QStringLiteral("O")},
        {QStringLiteral("name"), QStringLiteral("general")},
        {QStringLiteral("display_name"), QStringLiteral("General")},
    });
}

SidebarTeamState teamStateFor(const QString& channelId)
{
    SidebarTeamState result;
    SidebarCategory favorites;
    favorites.id = QStringLiteral("favorites-id");
    favorites.type = QStringLiteral("favorites");
    favorites.teamId = QStringLiteral("team");
    favorites.displayName = QStringLiteral("Favorites");

    SidebarCategory channels;
    channels.id = QStringLiteral("channels-id");
    channels.type = QStringLiteral("channels");
    channels.teamId = QStringLiteral("team");
    channels.channelIds = QStringList{channelId};

    SidebarCategory custom;
    custom.id = QStringLiteral("custom-id");
    custom.type = QStringLiteral("custom");
    custom.teamId = QStringLiteral("team");
    custom.displayName = QStringLiteral("My category");

    result.categories.insert(favorites.id, favorites);
    result.categories.insert(channels.id, channels);
    result.categories.insert(custom.id, custom);
    result.order = QStringList{favorites.id, channels.id, custom.id};
    return result;
}

} // namespace

class ConversationActionsMenuTest : public QObject
{
    Q_OBJECT

private slots:
    void channelCopyLinksUseOwningBackend()
    {
        Backend first;
        Backend second;
        first.networkSession().setHost(QStringLiteral("https://first.example.test/"));
        second.networkSession().setHost(QStringLiteral("https://second.example.test/"));
        BackendChannel* firstChannel = addTeamChannel(first, QStringLiteral("shared-channel"));
        BackendChannel* secondChannel = addTeamChannel(second, QStringLiteral("shared-channel"));
        QVERIFY(firstChannel);
        QVERIFY(secondChannel);

        const QString firstLink =
            ConversationActionsMenu::channelWebUrl(first, *firstChannel);
        const QString secondLink =
            ConversationActionsMenu::channelWebUrl(second, *secondChannel);
        QVERIFY(firstLink.startsWith(QStringLiteral("https://first.example.test/")));
        QVERIFY(secondLink.startsWith(QStringLiteral("https://second.example.test/")));
        QCOMPARE(firstLink.mid(QStringLiteral("https://first.example.test").size()),
                 secondLink.mid(QStringLiteral("https://second.example.test").size()));
    }

    void headerAndSidebarExposeExactlyTheSameActions()
    {
        Backend backend;
        const QString channelId = QStringLiteral("general-id");
        QVERIFY(addTeamChannel(backend, channelId));
        SidebarService::instance(backend).applyLocalTeamState(
            QStringLiteral("team"), teamStateFor(channelId));

        QWidget host;
        QMenu header(&host);
        QMenu sidebar(&host);
        ConversationActionsMenu::populate(
            header, backend, channelId, QStringLiteral("team"), &host);
        ConversationActionsMenu::populate(
            sidebar, backend, channelId, QStringLiteral("team"), &host);

        emit header.aboutToShow();
        emit sidebar.aboutToShow();
        QCOMPARE(actionSignature(header), actionSignature(sidebar));
        QVERIFY(actionNamed(header, QStringLiteral("editChannelTopicAction")));
        QVERIFY(actionNamed(sidebar, QStringLiteral("editChannelTopicAction")));
        QVERIFY(actionNamed(header, QStringLiteral("toggleConversationMuteAction")));
        QVERIFY(actionNamed(sidebar, QStringLiteral("toggleConversationFavoriteAction")));

        QMenu* headerCategories = actionNamed(
            header, QStringLiteral("moveConversationToCategoryMenu"))->menu();
        QMenu* sidebarCategories = actionNamed(
            sidebar, QStringLiteral("moveConversationToCategoryMenu"))->menu();
        QVERIFY(headerCategories && sidebarCategories);
        emit headerCategories->aboutToShow();
        emit sidebarCategories->aboutToShow();
        QCOMPARE(actionSignature(*headerCategories),
                 actionSignature(*sidebarCategories));
    }

    void directAndGroupConversationMenusAreIdenticalAcrossSurfaces()
    {
        Backend backend;
        QVERIFY(addTeamChannel(backend, QStringLiteral("normal")));
        Storage& storage = backend.getStorage();
        storage.addUser(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("me")},
            {QStringLiteral("username"), QStringLiteral("me")},
        }, true);
        storage.addUser(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("peer")},
            {QStringLiteral("username"), QStringLiteral("peer")},
        });
        QVERIFY(storage.addDirectChannel(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("direct-id")},
            {QStringLiteral("type"), QStringLiteral("D")},
            {QStringLiteral("name"), QStringLiteral("me__peer")},
        }));
        QVERIFY(storage.addGroupChannel(QJsonObject {
            {QStringLiteral("id"), QStringLiteral("group-id")},
            {QStringLiteral("type"), QStringLiteral("G")},
            {QStringLiteral("name"), QStringLiteral("group")},
            {QStringLiteral("display_name"), QStringLiteral("me, peer")},
        }));

        SidebarTeamState state = teamStateFor(QStringLiteral("normal"));
        SidebarCategory directs;
        directs.id = QStringLiteral("directs-id");
        directs.teamId = QStringLiteral("team");
        directs.type = QStringLiteral("direct_messages");
        directs.channelIds = QStringList{QStringLiteral("direct-id"), QStringLiteral("group-id")};
        state.categories.insert(directs.id, directs);
        state.order.append(directs.id);
        SidebarService::instance(backend).applyLocalTeamState(
            QStringLiteral("team"), state);

        QWidget host;
        for (const QString& id : {QStringLiteral("direct-id"),
                                  QStringLiteral("group-id")}) {
            QMenu header(&host);
            QMenu sidebar(&host);
            ConversationActionsMenu::populate(
                header, backend, id, QStringLiteral("team"), &host);
            ConversationActionsMenu::populate(
                sidebar, backend, id, QStringLiteral("team"), &host);
            emit header.aboutToShow();
            emit sidebar.aboutToShow();
            QCOMPARE(actionSignature(header), actionSignature(sidebar));
            QCOMPARE(actionNamed(header, QStringLiteral("viewConversationProfileAction"))
                         != nullptr,
                     id == QLatin1String("direct-id"));
            QVERIFY(actionNamed(header, QStringLiteral("toggleConversationFavoriteAction")));
            QVERIFY(actionNamed(sidebar, QStringLiteral("toggleConversationFavoriteAction")));
            auto* topic = actionNamed(header, QStringLiteral("editChannelTopicAction"));
            QVERIFY(!topic || !topic->isVisible());
            auto* convert = actionNamed(header, QStringLiteral("convertGroupMessageAction"));
            QCOMPARE(convert && convert->isVisible(), id == QLatin1String("group-id"));
        }
    }

    void conversionPreservesChannelIdentityAndUpdatesPersistentMenu()
    {
        Backend backend;
        QVERIFY(addTeamChannel(backend, QStringLiteral("normal")));
        auto& storage = backend.getStorage();
        storage.addUser(QJsonObject {{"id", "me"}, {"username", "me"}}, true);
        BackendChannel* group = storage.addGroupChannel(QJsonObject {
            {"id", "group-id"}, {"type", "G"}, {"name", "group"},
            {"display_name", "me, peer"}
        });
        QVERIFY(group);
        QPointer<BackendChannel> guard(group);
        BackendPost* post = group->addPost(QJsonObject {
            {"id", "post-id"}, {"user_id", "me"}, {"channel_id", "group-id"},
            {"message", "Preserve history"}, {"create_at", 1000}
        });
        QVERIFY(post);
        QWidget host;
        QMenu menu(&host);
        ConversationActionsMenu::populate(menu, backend, group->id, QStringLiteral("team"), &host);
        auto* convert = actionNamed(menu, QStringLiteral("convertGroupMessageAction"));
        auto* topic = actionNamed(menu, QStringLiteral("editChannelTopicAction"));
        QVERIFY(convert && convert->isVisible());
        QVERIFY(topic && !topic->isVisible());

        const QJsonObject converted {{"id", "group-id"}, {"team_id", "team"},
            {"type", "P"}, {"name", "private-project"}, {"display_name", "Private project"}};
        QJsonObject invalid = converted;
        invalid.insert(QStringLiteral("team_id"), QStringLiteral("unknown-team"));
        QVERIFY(!storage.applyConvertedGroupChannel(invalid));
        QCOMPARE(group->type, int(BackendChannel::groupChannel));
        QSignalSpy updates(group, &BackendChannel::onUpdated);
        QCOMPARE(backend.applyConvertedGroupChannel(converted), group);
        QVERIFY(guard);
        QCOMPARE(storage.getChannelById(QStringLiteral("group-id")), group);
        QCOMPARE(group->postIdToPost.value(QStringLiteral("post-id")), post);
        QCOMPARE(group->type, int(BackendChannel::privateChannel));
        QCOMPARE(group->team, storage.getTeamById(QStringLiteral("team")));
        QVERIFY(storage.groupChannels.channels.empty());
        QCOMPARE(group->team->channels.back().get(), group);
        QCOMPARE(updates.size(), 1);
        // A second delivery (REST after WebSocket) must not duplicate ownership.
        QCOMPARE(backend.applyConvertedGroupChannel(converted), group);
        QCOMPARE(updates.size(), 1);
        QCOMPARE(int(group->team->channels.size()), 2);
        emit menu.aboutToShow();
        QVERIFY(!convert->isVisible());
        QVERIFY(topic->isVisible());
    }

    void conversionUsesCommonTeamsAndPreservesStateOnRejectedRequest()
    {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray lastRequest;
        QJsonObject lastPayload;
        bool rejectConversion = true;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (server.hasPendingConnections()) {
                auto* socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    if (socket->property("answered").toBool()) return;
                    const QByteArray data = socket->property("input").toByteArray() + socket->readAll();
                    socket->setProperty("input", data);
                    const int end = data.indexOf("\r\n\r\n");
                    if (end < 0) return;
                    int length = 0;
                    for (const auto& line : data.left(end).split('\n')) {
                        if (line.toLower().startsWith("content-length:"))
                            length = line.mid(line.indexOf(':') + 1).trimmed().toInt();
                    }
                    if (data.size() < end + 4 + length) return;
                    socket->setProperty("answered", true);
                    lastRequest = data.left(data.indexOf('\r'));
                    lastPayload = QJsonDocument::fromJson(data.mid(end + 4, length)).object();
                    const bool common = lastRequest.startsWith("GET ")
                        && lastRequest.contains("/channels/group-id/common_teams");
                    const bool conversion = lastRequest.startsWith("POST ")
                        && lastRequest.contains("/channels/group-id/convert_to_channel");
                    const bool success = common || (conversion && !rejectConversion);
                    QJsonDocument doc;
                    if (common) {
                        doc = QJsonDocument(QJsonArray {QJsonObject {{"id", "team"}, {"name", "team"}}});
                    } else if (success) {
                        doc = QJsonDocument(QJsonObject {{"id", "group-id"}, {"team_id", "team"},
                            {"type", "P"}, {"name", "private-project"}, {"display_name", "Private project"}});
                    } else {
                        doc = QJsonDocument(QJsonObject {{"message", "Conversion denied"}});
                    }
                    const QByteArray body = doc.toJson(QJsonDocument::Compact);
                    socket->write(QByteArray(success ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 403 Forbidden\r\n")
                        + "Content-Type: application/json\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        Backend backend;
        backend.networkSession().setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        QVERIFY(addTeamChannel(backend, QStringLiteral("normal")));
        auto& storage = backend.getStorage();
        storage.addUser(QJsonObject {{"id", "me"}, {"username", "me"}}, true);
        BackendChannel* group = storage.addGroupChannel(QJsonObject {
            {"id", "group-id"}, {"type", "G"}, {"name", "group"}, {"display_name", "me, peer"}
        });
        QVERIFY(group);
        bool completed = false;
        QString error;
        QJsonArray teams;
        backend.retrieveGroupConversionTeams(group->id, [&](QJsonArray result, const QString& message) {
            teams = result; error = message; completed = true;
        });
        QTRY_VERIFY(completed);
        QVERIFY(error.isEmpty());
        QCOMPARE(teams.size(), 1);
        QCOMPARE(teams.first().toObject().value("id").toString(), QStringLiteral("team"));
        bool accepted = false;
        const auto submit = [&] {
            completed = false;
            backend.convertGroupMessageToChannel(group->id, QStringLiteral("team"),
                QStringLiteral("private-project"), QStringLiteral("Private project"),
                [&](bool success, const QString& message) {
                    accepted = success; error = message; completed = true;
                });
        };
        submit();
        QTRY_VERIFY(completed);
        QVERIFY(!accepted);
        QCOMPARE(error, QStringLiteral("Conversion denied"));
        QCOMPARE(group->type, int(BackendChannel::groupChannel));
        QCOMPARE(lastPayload.value("channel_id").toString(), QStringLiteral("group-id"));
        QCOMPARE(lastPayload.value("team_id").toString(), QStringLiteral("team"));
        QCOMPARE(lastPayload.value("name").toString(), QStringLiteral("private-project"));
        QCOMPARE(lastPayload.value("display_name").toString(), QStringLiteral("Private project"));
        rejectConversion = false;
        submit();
        QTRY_VERIFY(completed);
        QVERIFY(accepted);
        QCOMPARE(storage.getChannelById(QStringLiteral("group-id")), group);
        QCOMPARE(group->type, int(BackendChannel::privateChannel));
    }

    void openInNewTabRequestsTheClickedConversation()
    {
        Backend backend;
        const QString channelId = QStringLiteral("general-id");
        QVERIFY(addTeamChannel(backend, channelId));
        QWidget host;
        QMenu menu(&host);
        ConversationActionsMenu::populate(
            menu, backend, channelId, QStringLiteral("team"), &host);

        QAction* open = actionNamed(menu, QStringLiteral("openConversationInTabAction"));
        QVERIFY(open);
        auto& navigation = AppNavigationService::instance(backend);
        QSignalSpy tabs(&navigation, &AppNavigationService::tabRequested);
        QSignalSpy ordinaryNavigation(&navigation,
                                      &AppNavigationService::channelRequested);

        open->trigger();
        QCOMPARE(tabs.size(), 1);
        QCOMPARE(tabs.takeFirst().at(0).toString(), channelId);
        QCOMPARE(ordinaryNavigation.size(), 0);
    }

    void favoritesStatusAndIconUpdateInBothMenus()
    {
        Backend backend;
        const QString channelId = QStringLiteral("general-id");
        QVERIFY(addTeamChannel(backend, channelId));
        auto& sidebarState = SidebarService::instance(backend);
        SidebarTeamState state = teamStateFor(channelId);
        sidebarState.applyLocalTeamState(QStringLiteral("team"), state);

        QWidget host;
        QMenu header(&host);
        QMenu sidebar(&host);
        ConversationActionsMenu::populate(
            header, backend, channelId, QStringLiteral("team"), &host);
        ConversationActionsMenu::populate(
            sidebar, backend, channelId, QStringLiteral("team"), &host);

        QAction* headFavorite = actionNamed(
            header, QStringLiteral("toggleConversationFavoriteAction"));
        QAction* sideFavorite = actionNamed(
            sidebar, QStringLiteral("toggleConversationFavoriteAction"));
        QVERIFY(headFavorite && sideFavorite);
        emit header.aboutToShow();
        emit sidebar.aboutToShow();
        QCOMPARE(headFavorite->text(), QStringLiteral("Add to Favorites"));
        QCOMPARE(sideFavorite->text(), headFavorite->text());
        QCOMPARE(headFavorite->property("matterleastMenuIconPath").toString(),
                 QStringLiteral(":/icons/heart-outline"));

        state.categoryByType(QStringLiteral("channels"))->channelIds.clear();
        state.categoryByType(QStringLiteral("favorites"))->channelIds.append(channelId);
        sidebarState.applyLocalTeamState(QStringLiteral("team"), state);
        emit header.aboutToShow();
        emit sidebar.aboutToShow();
        QCOMPARE(headFavorite->text(), QStringLiteral("Remove from Favorites"));
        QCOMPARE(sideFavorite->text(), headFavorite->text());
        QCOMPARE(headFavorite->property("matterleastMenuIconPath").toString(),
                 QStringLiteral(":/icons/heart-filled"));
        QCOMPARE(sideFavorite->property("matterleastMenuIconPath").toString(),
                 headFavorite->property("matterleastMenuIconPath").toString());
    }
};

QTEST_MAIN(ConversationActionsMenuTest)
#include "ConversationActionsMenuTest.moc"
