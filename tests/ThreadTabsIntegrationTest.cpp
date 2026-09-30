#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTabBar>
#include <QStackedWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrlQuery>

#include "backend/Backend.h"
#include "backend/FollowingModel.h"
#include "backend/SidebarService.h"
#include "backend/NetworkRequest.h"
#include "backend/PostRepository.h"
#include "backend/types/BackendChannel.h"
#include "channel-tree/ChannelTree.h"
#include "chat-area/ChatArea.h"
#include "chat-area/ChatLogWidget.h"
#include "chat-area/ThreadPostSource.h"
#include "mainwindow.h"
#include "navigation/AppNavigationService.h"
#include "navigation/NavigationUiController.h"
#include "options/MLOptions.h"

using namespace Mattermost;

namespace {
QString rootId(int thread) { return QStringLiteral("root%1").arg(thread, 22, 10, QLatin1Char('0')); }
QString replyId(int thread, int reply) { return QStringLiteral("reply%1_%2").arg(thread).arg(reply); }
QString sessionKey(const QString& user = QStringLiteral("user"))
{
    return QStringLiteral("navigation_session/") + QString::fromLatin1(
        QCryptographicHash::hash((NetworkRequest::host() + QChar(0x1f) + user).toUtf8(),
                                QCryptographicHash::Sha256).toHex());
}
QJsonObject post(int thread, int reply = 0)
{
    const qint64 time = 1000000000000LL + thread * 1000 + reply;
    QJsonObject result {{"id", reply ? replyId(thread, reply) : rootId(thread)},
                        {"channel_id", "channel"}, {"user_id", "user"},
                        {"root_id", reply ? rootId(thread) : QString()},
                        {"message", QStringLiteral("Thread %1, post %2").arg(thread).arg(reply)},
                        {"create_at", time}, {"update_at", time}};
    if (!reply) {
        result.insert("reply_count", 3);
        result.insert("last_reply_at", time + 3);
    }
    return result;
}

class Server : public QTcpServer
{
public:
    QStringList postLookups;
    QStringList threadLookups;
    QStringList teamChannelLookups;
    int teamChannelResponses = 0;
    int delayMs = 0;
    int replyCount = 3;
    int returnedReplies = 0;
    bool failPost = false;
    bool startupChannels = false;
    Server()
    {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const QByteArray input = socket->property("input").toByteArray() + socket->readAll();
                    socket->setProperty("input", input);
                    if (!input.contains("\r\n\r\n") || socket->property("replied").toBool()) return;
                    socket->setProperty("replied", true);
                    const QUrl url(QString::fromUtf8(input.split(' ').value(1)));
                    const QStringList parts = url.path().split('/', Qt::SkipEmptyParts);
                    QByteArray body = "[]";
                    bool failed = false;
                    const bool teamChannels = url.path().startsWith("/api/v4/users/me/teams/")
                        && url.path().endsWith("/channels");
                    if (teamChannels) teamChannelLookups.push_back(url.path());
                    if (startupChannels && url.path() == "/api/v4/users/me/teams") {
                        body = R"([{"id":"team","name":"team","display_name":"Team"}])";
                    } else if (startupChannels && url.path() == "/api/v4/users/me/teams/team/channels") {
                        body = R"([{"id":"channel","type":"G","display_name":"Test chat"}])";
                    } else if (startupChannels && url.path().endsWith("/channels/categories")) {
                        body = R"({"categories":[{"id":"dm","team_id":"team","type":"direct_messages","display_name":"Direct Messages","channel_ids":["channel"]}],"order":["dm"]})";
                    }
                    if (parts.size() >= 4 && parts.at(2) == "posts") {
                        const QString root = parts.at(3);
                        const int thread = root.mid(4).toInt();
                        QJsonObject rootPost = post(thread);
                        rootPost.insert("reply_count", replyCount);
                        rootPost.insert("last_reply_at", 1000000000000LL + thread * 1000 + replyCount);
                        if (parts.size() == 4) {
                            postLookups.push_back(root);
                            failed = failPost;
                            if (root.startsWith("reply")) {
                                const auto ids = root.mid(5).split('_');
                                rootPost = post(ids.value(0).toInt(), ids.value(1).toInt());
                            }
                            body = QJsonDocument(rootPost).toJson(QJsonDocument::Compact);
                        } else if (parts.at(4) == "thread") {
                            threadLookups.push_back(root);
                            QJsonObject posts {{root, rootPost}};
                            QJsonArray order {root};
                            const QUrlQuery query(url);
                            const qint64 cursor = query.queryItemValue("fromCreateAt").toLongLong();
                            const bool backward = query.queryItemValue("direction") == "up";
                            QList<int> selected;
                            for (int i = 1; i <= replyCount; ++i) {
                                const qint64 time = post(thread, i).value("create_at").toVariant().toLongLong();
                                if (!cursor || (backward ? time < cursor : time > cursor)) selected.push_back(i);
                            }
                            if (backward) std::reverse(selected.begin(), selected.end());
                            const int limit = query.queryItemValue("perPage").toInt();
                            const bool hasNext = selected.size() > limit;
                            selected = selected.mid(0, limit);
                            returnedReplies += selected.size();
                            for (int i : selected) {
                                posts.insert(replyId(thread, i), post(thread, i));
                                order.append(replyId(thread, i));
                            }
                            body = QJsonDocument(QJsonObject {{"posts", posts}, {"order", order},
                                                             {"has_next", hasNext}}).toJson(QJsonDocument::Compact);
                        }
                    }
                    const QByteArray response = "HTTP/1.1 " + QByteArray(failed ? "500 Failed" : "200 OK")
                        + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body;
                    QTimer::singleShot(delayMs, socket, [this, socket, response, teamChannels] {
                        socket->write(response);
                        socket->disconnectFromHost();
                        if (teamChannels) ++teamChannelResponses;
                    });
                });
            }
        });
    }
};
}

class ThreadTabsIntegrationTest : public QObject
{
    Q_OBJECT
private slots:
    void repositoryShutdownReleasesCallbackOwnedLeases()
    {
        Server server;
        server.delayMs = 50;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        auto& storage = backend.getStorage();
        storage.addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* channel = storage.addGroupChannel(QJsonObject {{"id", "channel"}, {"type", "G"}});
        auto* root = channel->addPost(post(300));
        auto& repository = PostRepository::instance(backend);
        auto lease = std::make_shared<PostResidencyLease>(repository.leasePost(*root));
        std::weak_ptr<PostResidencyLease> callbackLease(lease);
        int delivered = 0;
        repository.loadThreadTail(*channel, root->id, 10,
            [lease, &delivered](const PostRepository::Page&) { ++delivered; });
        lease.reset();
        QVERIFY(!callbackLease.expired());
        QVERIFY(repository.isPostLeased(channel->id, root->id));
        delete &repository;
        QVERIFY(callbackLease.expired());
        QCOMPARE(delivered, 0);
        QCOMPARE(channel->postIdToPost.value(root->id), root);
    }

    void lateTeamChannelsResponseDoesNotTouchReplacementTeam()
    {
        Server server;
        server.startupChannels = true;
        server.delayMs = 50;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        auto& storage = backend.getStorage();
        storage.addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* team = storage.addTeam(QJsonObject {{"id", "team"}, {"name", "old"}});
        int delivered = 0;
        backend.retrieveOwnChannelMembershipsForTeam(*team, [&delivered](BackendChannel&) { ++delivered; });
        QTRY_COMPARE(server.teamChannelLookups.size(), 1);
        storage.teams.clear();
        auto* replacement = storage.addTeam(QJsonObject {{"id", "team"}, {"name", "replacement"}});
        auto* retained = storage.addTeamChannel(*replacement, QJsonObject {{"id", "retained"}, {"type", "O"}});
        QSignalSpy populated(&backend, &Backend::onAllTeamChannelsPopulated);
        QTRY_COMPARE(server.teamChannelResponses, 1);
        // Drain the HTTP completion, not merely the server's socket write.
        QTest::qWait(100);
        QCOMPARE(delivered, 0);
        QCOMPARE(populated.count(), 0);
        QCOMPARE(replacement->channels.size(), size_t(1));
        QCOMPARE(replacement->channels.front().get(), retained);
        QCOMPARE(storage.getChannelById("retained"), retained);
    }

    void sessionRestoresTabOrderDockAndDetachedWindow()
    {
        Server server;
        server.startupChannels = true;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        const QString key = sessionKey();
        MLOptions::instance()->setValue(key, QByteArray());
        QSystemTrayIcon tray;
        {
            Backend backend;
            backend.getStorage().addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
            MainWindow window(nullptr, tray, backend);
            window.show();
            QTRY_VERIFY(window.findChild<QTabBar*>(QStringLiteral("navigationTabs")));
            QTRY_VERIFY(backend.getStorage().getChannelById("channel"));
            QTRY_VERIFY(SidebarService::instance(backend).teamState("team")
                        && SidebarService::instance(backend).teamState("team")->category("dm"));
            auto& ui = NavigationUiController::instance(window);
            auto& navigation = AppNavigationService::instance(backend);
            navigation.openChannelInTab("channel");
            navigation.openThreadInTab("channel", rootId(201));
            navigation.openThreadInTab("channel", rootId(202));
            auto* tabs = window.findChild<QTabBar*>(QStringLiteral("navigationTabs"));
            QCOMPARE(tabs->count(), 3);
            tabs->moveTab(2, 1);
            tabs->setCurrentIndex(1);
            auto* channel = backend.getStorage().getChannelById("channel");
            QVERIFY(channel);
            auto* dock = new ChatArea(backend, *channel, rootId(203), nullptr);
            ui.presentThread(dock);
            auto* detached = new ChatArea(backend, *channel, rootId(204), nullptr);
            detached->setProperty("threadDetached", true);
            detached->resize(640, 480);
            ui.presentThread(detached);
            QTRY_VERIFY(channel->postIdToPost.contains(rootId(204)));
            tabs->setCurrentIndex(1);
            ui.saveSession();
            const auto saved = QJsonDocument::fromJson(MLOptions::instance()->value<QByteArray>(key)).object();
            QCOMPARE(saved.value("tabs").toArray().size(), 3);
            QCOMPARE(saved.value("tabs").toArray().at(1).toObject().value("root").toString(), rootId(202));
            QCOMPARE(saved.value("active_tab").toObject().value("root").toString(), rootId(202));
            QCOMPARE(saved.value("threads").toArray().size(), 2);
            // The same process/account cache is not used by the next Backend.
            delete detached;
        }
        {
            Backend backend;
            backend.getStorage().addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
            MainWindow window(nullptr, tray, backend);
            window.show();
            // No manual restore call: exercise the real startup readiness path.
            QTRY_VERIFY(NavigationUiController::instance(window).findThread("channel", rootId(204)));
            auto& ui = NavigationUiController::instance(window);
            auto* tabs = window.findChild<QTabBar*>(QStringLiteral("navigationTabs"));
            QCOMPARE(tabs->count(), 3);
            QCOMPARE(tabs->currentIndex(), 1);
            QVERIFY(ui.findThread("channel", rootId(202))->property("threadTabbed").toBool());
            auto* dock = ui.findThread("channel", rootId(203));
            QVERIFY(dock);
            auto* stack = window.findChild<QStackedWidget*>(QStringLiteral("threadStack"));
            QCOMPARE(stack->currentWidget(), dock);
            QVERIFY(!stack->isHidden());
            auto* detached = ui.findThread("channel", rootId(204));
            QVERIFY(detached->isWindow());
            QVERIFY(detached->property("threadDetached").toBool());
            QCOMPARE(detached->size(), QSize(640, 480));
            QTRY_VERIFY(backend.getStorage().getChannelById("channel")->postIdToPost.contains(rootId(202)));
            ui.saveSession();
            const auto saved = QJsonDocument::fromJson(MLOptions::instance()->value<QByteArray>(key)).object();
            QCOMPARE(saved.value("tabs").toArray().at(2).toObject().value("root").toString(), rootId(201));
            QTest::mouseClick(tabs, Qt::MiddleButton, Qt::NoModifier, tabs->tabRect(2).center());
            QTRY_COMPARE(tabs->count(), 2);
            ui.saveSession();
            QCOMPARE(QJsonDocument::fromJson(MLOptions::instance()->value<QByteArray>(key))
                         .object().value("tabs").toArray().size(), 2);
            delete detached;
        }
    }

    void sessionColdBookmarkAndUserNavigation()
    {
        Server server;
        server.startupChannels = true;
        server.replyCount = 731;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        const QJsonObject destination {{"channel", "channel"}, {"root", rootId(205)},
                                       {"post", replyId(205, 365)}};
        const QJsonObject snapshot {{"version", 1}, {"tabs", QJsonArray {destination}},
                                    {"active_tab", destination}};
        MLOptions::instance()->setValue(sessionKey(), QJsonDocument(snapshot).toJson());
        Backend backend;
        backend.getStorage().addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        QSystemTrayIcon tray;
        MainWindow window(nullptr, tray, backend);
        window.show();
        auto& ui = NavigationUiController::instance(window);
        QTRY_VERIFY(ui.findThread("channel", rootId(205)));
        auto* area = ui.findThread("channel", rootId(205));
        auto* log = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QTRY_VERIFY(server.postLookups.contains(replyId(205, 365)));
        QTRY_VERIFY(area->property("sessionBookmark").toString().isEmpty());
        QTRY_VERIFY(!log->isAtEnd());
        const int index = log->source()->indexOfPost(replyId(205, 365));
        QVERIFY(index >= 0);
        QTRY_VERIFY(log->itemWidget(index));
        QVERIFY(log->visibleRange().first <= index && index <= log->visibleRange().last);
        QVERIFY(server.returnedReplies < 100);
        // Explicit navigation cancels pending passive state immediately.
        area->setProperty("sessionBookmark", replyId(205, 100));
        area->goToNewest();
        QVERIFY(area->property("sessionBookmark").toString().isEmpty());
        QTRY_VERIFY(log->isAtEnd());
        ui.saveSession();
        const auto saved = QJsonDocument::fromJson(MLOptions::instance()->value<QByteArray>(sessionKey())).object();
        QVERIFY(saved.value("tabs").toArray().at(0).toObject().value("post").toString().isEmpty());
        QVERIFY(!MLOptions::instance()->contains(sessionKey("other-user")));
    }

    void sessionDoesNotRestoreAnotherAccount()
    {
        Server server;
        server.startupChannels = true;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        const QJsonObject destination {{"channel", "channel"}, {"root", rootId(299)}};
        const QByteArray original = QJsonDocument(QJsonObject {{"version", 1},
            {"tabs", QJsonArray {destination}}, {"active_tab", destination}}).toJson();
        MLOptions::instance()->setValue(sessionKey(), original);
        Backend backend;
        backend.getStorage().addUser(QJsonObject {{"id", "other-user"}, {"username", "other"}}, true);
        QSystemTrayIcon tray;
        MainWindow window(nullptr, tray, backend);
        window.show();
        QTRY_VERIFY(SidebarService::instance(backend).teamState("team")
                    && SidebarService::instance(backend).teamState("team")->category("dm"));
        auto& ui = NavigationUiController::instance(window);
        QVERIFY(!ui.findThread("channel", rootId(299)));
        ui.saveSession();
        QCOMPARE(MLOptions::instance()->value<QByteArray>(sessionKey()), original);
        QVERIFY(MLOptions::instance()->contains(sessionKey("other-user")));
    }

    void threadTabAttentionIsIndependentFromParentChannel()
    {
        Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        auto& storage = backend.getStorage();
        storage.addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* team = storage.addTeam(QJsonObject {{"id", "team"}, {"name", "team"}});
        auto* channel = storage.addGroupChannel(QJsonObject {{"id", "channel"}, {"type", "G"},
                                                            {"display_name", "Test chat"}});
        auto* root = channel->addPost(post(101));
        auto* sibling = channel->addPost(post(102));
        auto& model = FollowingModel::instance(backend);
        model.markPostUnread(channel->id, root->id, root->id, root->create_at);
        model.observeReadThrough(channel->id, root->id, *root, true);
        model.markThreadRead(team->id, root->id);
        QVERIFY(!model.findEntry(channel->id, root->id)->requiresAttention());
        model.markPostUnread(channel->id, sibling->id, sibling->id, sibling->create_at);

        auto& sidebar = SidebarService::instance(backend);
        QSystemTrayIcon tray;
        MainWindow window(nullptr, tray, backend);
        window.findChild<ChannelTree*>(QStringLiteral("channelList"))->addTeam(backend, *team);
        NavigationUiController::instance(window);
        sidebar.setChannelMentioned(channel->id, true);
        auto& navigation = AppNavigationService::instance(backend);
        navigation.openThreadInTab(channel->id, root->id);
        navigation.openThreadInTab(channel->id, sibling->id);
        auto* tabs = window.findChild<QTabBar*>(QStringLiteral("navigationTabs"));
        QVERIFY(tabs);
        QCOMPARE(tabs->count(), 2);
        QVERIFY(!tabs->tabText(0).startsWith(QStringLiteral("★ ")));
        QVERIFY(tabs->tabText(1).startsWith(QStringLiteral("★ ")));

        // Parent activity updates must neither mark a read child nor clear
        // another child's independent unread marker.
        sidebar.setChannelMentioned(channel->id, false);
        QVERIFY(!tabs->tabText(0).startsWith(QStringLiteral("★ ")));
        QVERIFY(tabs->tabText(1).startsWith(QStringLiteral("★ ")));

        // Model-only changes must refresh tab titles without any parent event.
        model.markPostUnread(channel->id, root->id, root->id, root->create_at);
        QVERIFY(tabs->tabText(0).startsWith(QStringLiteral("★ ")));
        model.observeReadThrough(channel->id, root->id, *root, true);
        model.markThreadRead(team->id, root->id);
        QVERIFY(!tabs->tabText(0).startsWith(QStringLiteral("★ ")));
        QVERIFY(tabs->tabText(1).startsWith(QStringLiteral("★ ")));
    }

    void coldThreadsOpenInSuccessiveTabs()
    {
        Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        auto& storage = backend.getStorage();
        storage.addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* team = storage.addTeam(QJsonObject {{"id", "team"}, {"name", "team"}});
        auto* channel = storage.addGroupChannel(QJsonObject {{"id", "channel"}, {"type", "G"},
                                                            {"display_name", "Test chat"}});
        QVERIFY(channel);
        channel->addPost(post(1)); // Only the first Following entry has a resident root.
        QSystemTrayIcon tray;
        MainWindow window(nullptr, tray, backend);
        window.findChild<ChannelTree*>(QStringLiteral("channelList"))->addTeam(backend, *team);
        auto& ui = NavigationUiController::instance(window);
        window.resize(1000, 700);
        window.show();
        auto& navigation = AppNavigationService::instance(backend);
        for (int thread = 1; thread <= 8; ++thread) {
            navigation.openThreadInTab(channel->id, rootId(thread));
            QTRY_VERIFY(ui.findThread(channel->id, rootId(thread)));
            auto* area = ui.findThread(channel->id, rootId(thread));
            auto* list = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
            QVERIFY(list);
            QTRY_COMPARE(list->itemCount(), 4);
            QTRY_VERIFY(list->findPost(replyId(thread, 3)));
            QVERIFY(area->isVisible());
            QVERIFY(area->property("threadTabbed").toBool());
            for (int previous = 1; previous < thread; ++previous) {
                auto* existing = ui.findThread(channel->id, rootId(previous));
                QVERIFY(existing);
                QVERIFY(!existing->isVisible());
            }
        }
        QCOMPARE(server.postLookups.size(), 7);
        QVERIFY(!server.postLookups.contains(rootId(1)));

        // Tab presentation must not cancel other sources' bootstrap when the
        // user opens several Following entries before any HTTP reply arrives.
        server.delayMs = 50;
        for (int thread = 21; thread <= 28; ++thread) {
            navigation.openThreadInTab(channel->id, rootId(thread));
            QVERIFY(ui.findThread(channel->id, rootId(thread)));
        }
        for (int thread = 21; thread <= 28; ++thread) {
            auto* area = ui.findThread(channel->id, rootId(thread));
            auto* list = area->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
            QTRY_COMPARE(list->itemCount(), 4);
            QTRY_VERIFY(list->findPost(replyId(thread, 3)));
            QCOMPARE(area->isVisible(), thread == 28);
        }
        QCOMPARE(server.postLookups.size(), 15);
    }

    void missingRootFailureCanBeRetried()
    {
        Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        backend.getStorage().addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* channel = backend.getStorage().addGroupChannel(
            QJsonObject {{"id", "channel"}, {"type", "G"}});
        ThreadPostSource source(backend, *channel, rootId(90));
        QCOMPARE(source.itemCount(), 1);
        QCOMPARE(source.indexOfPost(rootId(90)), 0);
        QVERIFY(!source.isAvailable(0));
        QSignalSpy finished(&source, &AbstractPostSource::rangeRequestFinished);
        QSignalSpy failed(&source, &ThreadPostSource::rangeRequestFailed);
        server.failPost = true;
        source.requestRange(0, 0, AbstractPostSource::RequestReason::EnsureVisible, 0);
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(source.itemCount(), 1);
        QVERIFY(!source.isAvailable(0));
        server.failPost = false;
        source.requestRange(0, 0, AbstractPostSource::RequestReason::EnsureVisible, 0);
        QTRY_COMPARE(finished.size(), 2);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(source.itemCount(), 4);
        QVERIFY(source.isAvailable(0));
        QCOMPARE(server.postLookups.size(), 2);
        QCOMPARE(server.threadLookups.size(), 0); // Root demand does not enumerate replies.
    }

    void rootDeliveredByAnotherRequestResolvesSummary()
    {
        Backend backend;
        backend.getStorage().addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* channel = backend.getStorage().addGroupChannel(
            QJsonObject {{"id", "channel"}, {"type", "G"}});
        const QString root = rootId(94);
        {
            ThreadPostSource source(backend, *channel, root);
            QCOMPARE(source.itemCount(), 1);
            channel->addPost(post(94));
            QCOMPARE(source.itemCount(), 4);
            QVERIFY(source.isAvailable(0));
            QVERIFY(PostRepository::instance(backend).isPostLeased(channel->id, root));
        }
        QVERIFY(!PostRepository::instance(backend).isPostLeased(channel->id, root));
    }

    void coldThreadSummaryPreservesNavigationIntent_data()
    {
        QTest::addColumn<bool>("explicitRoot");
        QTest::newRow("newest") << false;
        QTest::newRow("explicit-root") << true;
    }

    void coldThreadSummaryPreservesNavigationIntent()
    {
        QFETCH(bool, explicitRoot);
        Server server;
        server.delayMs = 20;
        server.replyCount = 731;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        backend.getStorage().addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* channel = backend.getStorage().addGroupChannel(
            QJsonObject {{"id", "channel"}, {"type", "G"}});
        const int thread = explicitRoot ? 93 : 92;
        ChatArea area(backend, *channel, rootId(thread), nullptr);
        area.resize(1000, 700);
        area.show();
        auto* list = area.findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        if (explicitRoot) {
            area.preparePostNavigation();
            QVERIFY(area.lockNavigationToPost(rootId(thread), 0));
        } else {
            area.goToNewest(); // Same intent as opening a fresh docked thread.
        }
        QTRY_COMPARE(list->itemCount(), 732);
        if (explicitRoot) {
            QTRY_VERIFY(list->findPost(rootId(thread)));
            QCOMPARE(list->visibleRange().first, 0);
        } else {
            QTRY_VERIFY(list->findPost(replyId(thread, 731)));
            QVERIFY(list->isAtEnd());
        }
        QCOMPARE(server.postLookups.size(), 1);
        QVERIFY(server.returnedReplies < 100);
    }

    void closingSourceDuringRootRetrievalIsSafe()
    {
        Server server;
        server.delayMs = 50;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        backend.getStorage().addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* channel = backend.getStorage().addGroupChannel(
            QJsonObject {{"id", "channel"}, {"type", "G"}});
        auto source = std::make_unique<ThreadPostSource>(backend, *channel, rootId(91));
        source->requestRange(0, 0, AbstractPostSource::RequestReason::EnsureVisible, 0);
        QTRY_COMPARE(server.postLookups.size(), 1);
        source.reset();
        QTRY_VERIFY(channel->postIdToPost.contains(rootId(91)));
        QCOMPARE(server.threadLookups.size(), 0);
    }
};

int main(int argc, char** argv)
{
    QTemporaryDir state;
    qputenv("XDG_CACHE_HOME", state.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", state.path().toUtf8());
    qputenv("XDG_DATA_HOME", state.path().toUtf8());
    QApplication app(argc, argv);
    QStandardPaths::setTestModeEnabled(true);
    QLoggingCategory::setFilterRules(QStringLiteral("*.debug=false\n"));
    ThreadTabsIntegrationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ThreadTabsIntegrationTest.moc"
