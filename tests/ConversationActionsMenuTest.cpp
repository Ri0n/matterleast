#include <QtTest>

#include <QAction>
#include <QJsonObject>
#include <QMenu>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
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
    void initTestCase()
    {
        NetworkRequest::setHost(QStringLiteral("https://mattermost.example.test/"));
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
            QVERIFY(!actionNamed(header, QStringLiteral("editChannelTopicAction")));
        }
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
