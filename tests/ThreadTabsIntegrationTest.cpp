#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QScrollBar>
#include <QScopedPointer>
#include <QTabBar>
#include <QToolButton>
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
QString channelPostId(int postIndex) { return QStringLiteral("channel_post_%1").arg(postIndex); }
QJsonObject channelPost(int postIndex)
{
    const qint64 time = 1000000000000LL + postIndex * 1000;
    return QJsonObject {{"id", channelPostId(postIndex)},
                        {"channel_id", "channel"}, {"user_id", "user"},
                        {"root_id", ""},
                        {"message", QStringLiteral("Channel post %1").arg(postIndex)},
                        {"create_at", time}, {"update_at", time}};
}
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
    QStringList channelContextRequests;
    int channelRootCount = 0;
    int teamChannelResponses = 0;
    int delayMs = 0;
    int replyCount = 3;
    int returnedReplies = 0;
    bool failPost = false;
    bool startupChannels = false;
    bool secondChannel = false;
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
                        QJsonObject info {{"id", "channel"}, {"type", "G"},
                                          {"display_name", "Test chat"}};
                        if (channelRootCount > 0) {
                            info.insert("total_msg_count_root", channelRootCount);
                            info.insert("total_msg_count", channelRootCount);
                            info.insert("last_root_post_at",
                                        channelPost(channelRootCount).value("create_at"));
                        }
                        QJsonArray channels {info};
                        if (secondChannel) {
                            channels.append(QJsonObject {
                                {"id", "channel_other"}, {"type", "G"},
                                {"display_name", "Other chat"}});
                        }
                        body = QJsonDocument(channels).toJson(QJsonDocument::Compact);
                    } else if (startupChannels && url.path().endsWith("/channels/categories")) {
                        body = secondChannel
                            ? R"({"categories":[{"id":"dm","team_id":"team","type":"direct_messages","display_name":"Direct Messages","channel_ids":["channel","channel_other"]}],"order":["dm"]})"
                            : R"({"categories":[{"id":"dm","team_id":"team","type":"direct_messages","display_name":"Direct Messages","channel_ids":["channel"]}],"order":["dm"]})";
                    }
                    if (channelRootCount > 0
                        && url.path() == "/api/v4/channels/channel/posts") {
                        const QUrlQuery query(url);
                        const QString before = query.queryItemValue("before");
                        const QString after = query.queryItemValue("after");
                        if (!before.isEmpty() || !after.isEmpty()) {
                            channelContextRequests.push_back(before.isEmpty() ? after : before);
                        }
                        int first = 1;
                        int last = channelRootCount;
                        if (!before.isEmpty()) {
                            last = std::min(last,
                                before.mid(QStringLiteral("channel_post_").size()).toInt() - 1);
                        } else if (!after.isEmpty()) {
                            first = std::max(first,
                                after.mid(QStringLiteral("channel_post_").size()).toInt() + 1);
                        }
                        const int perPage = std::max(1,
                            query.queryItemValue("per_page").toInt());
                        const int page = std::max(0,
                            query.queryItemValue("page").toInt());
                        QJsonObject posts;
                        QJsonArray order;
                        int skipped = page * perPage;
                        int returned = 0;
                        for (int i = last; i >= first && returned < perPage; --i) {
                            if (skipped-- > 0) continue;
                            posts.insert(channelPostId(i), channelPost(i));
                            order.append(channelPostId(i));
                            ++returned;
                        }
                        body = QJsonDocument(QJsonObject {
                            {"posts", posts}, {"order", order},
                            {"prev_post_id", first > 1 ? channelPostId(first - 1) : QString()},
                            {"next_post_id", last < channelRootCount
                                ? channelPostId(last + 1) : QString()}
                        }).toJson(QJsonDocument::Compact);
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
                            if (root.startsWith("channel_post_")) {
                                rootPost = channelPost(
                                    root.mid(QStringLiteral("channel_post_").size()).toInt());
                            } else if (root.startsWith("reply")) {
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

    void replacingDockDisposesOldThreadView()
    {
        Server server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/")
                                    .arg(server.serverPort()));
        // Explicitly establish a blank persisted session for this isolated
        // fixture. saveSession() otherwise intentionally refuses to write
        // before the one-time restoreSession() readiness gate has run.
        MLOptions::instance()->setValue(sessionKey(), QByteArray());
        Backend backend;
        auto& storage = backend.getStorage();
        storage.addUser(
            QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* channel = storage.addGroupChannel(
            QJsonObject {{"id", "channel"}, {"type", "G"}});
        QVERIFY(channel);
        QSystemTrayIcon tray;
        MainWindow window(nullptr, tray, backend);
        window.show();

        auto& controller = NavigationUiController::instance(window);
        controller.restoreSession();
        QPointer<ChatArea> old = new ChatArea(backend, *channel, rootId(401), nullptr);
        controller.presentThread(old);
        QVERIFY(old);
        auto* replacement = new ChatArea(backend, *channel, rootId(402), nullptr);
        controller.presentThread(replacement);
        QTRY_VERIFY(old.isNull());
        QCOMPARE(controller.findThread("channel", rootId(402)), replacement);
        auto* stack = window.findChild<QStackedWidget*>(
            QStringLiteral("threadStack"));
        QVERIFY(stack);
        QCOMPARE(stack->count(), 1);
        controller.saveSession();
        const QJsonObject persisted = QJsonDocument::fromJson(
            MLOptions::instance()->value<QByteArray>(sessionKey())).object();
        QCOMPARE(persisted.value("threads").toArray().size(), 1);
    }

    void startupOnlyMaterializesSelectedTabAndVisibleDock()
    {
        Server server;
        server.startupChannels = true;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/")
                                    .arg(server.serverPort()));
        const QJsonObject tab1 {{"channel", "channel"}, {"root", rootId(301)},
                                {"post", replyId(301, 2)}};
        const QJsonObject tab2 {{"channel", "channel"}, {"root", rootId(302)}};
        const QJsonObject oldDock {{"channel", "channel"}, {"root", rootId(303)},
                                   {"mode", "dock"}};
        const QJsonObject activeDock {{"channel", "channel"}, {"root", rootId(304)},
                                      {"mode", "dock"}};
        const QJsonObject snapshot {{"version", 1},
            {"tabs", QJsonArray {tab1, tab2}}, {"active_tab", tab2},
            {"central", QJsonObject {{"channel", "channel"}}},
            {"threads", QJsonArray {oldDock, activeDock}},
            {"active_dock", activeDock}, {"dock_visible", true}};
        MLOptions::instance()->setValue(
            sessionKey(), QJsonDocument(snapshot).toJson(QJsonDocument::Compact));

        Backend backend;
        backend.getStorage().addUser(
            QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        QSystemTrayIcon tray;
        MainWindow window(nullptr, tray, backend);
        window.show();
        auto& ui = NavigationUiController::instance(window);
        auto* tabs = window.findChild<QTabBar*>(QStringLiteral("navigationTabs"));
        QVERIFY(tabs);
        QTRY_VERIFY(ui.findThread("channel", rootId(302)));
        QTRY_VERIFY(ui.findThread("channel", rootId(304)));
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->currentIndex(), 1);
        QVERIFY(!ui.findThread("channel", rootId(301)));
        QVERIFY(!ui.findThread("channel", rootId(303)));

        ui.saveSession();
        const QJsonObject persisted = QJsonDocument::fromJson(
            MLOptions::instance()->value<QByteArray>(sessionKey())).object();
        QCOMPARE(persisted.value("tabs").toArray().size(), 2);
        QCOMPARE(persisted.value("threads").toArray().size(), 1);
        QVERIFY(!persisted.contains("central")); // Hidden channel is not a session surface.
        QVERIFY(!persisted.value("tabs").toArray().at(0).toObject().contains("post"));
        QVERIFY(!persisted.value("threads").toArray().at(0).toObject().contains("post"));

        // The dormant tab is constructed only when the user selects it.
        tabs->setCurrentIndex(0);
        QTRY_VERIFY(ui.findThread("channel", rootId(301)));
    }

    void coldChannelSessionRestoresPostRelativeViewport()
    {
        Server server;
        server.startupChannels = true;
        server.channelRootCount = 48;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/")
                                    .arg(server.serverPort()));

        const QString targetId = channelPostId(24);
        const QJsonObject savedAnchor {
            {"post", targetId}, {"bottom", false}, {"fraction", 0.0}};
        const QJsonObject destination {
            {"channel", "channel"}, {"root", ""},
            {"anchor", savedAnchor}};
        MLOptions::instance()->setValue(sessionKey(),
            QJsonDocument(QJsonObject {
                {"version", 2}, {"tabs", QJsonArray {destination}},
                {"active_tab", destination}})
                .toJson(QJsonDocument::Compact));

        Backend backend;
        backend.getStorage().addUser(
            QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        QSystemTrayIcon tray;
        MainWindow window(nullptr, tray, backend);
        window.resize(1000, 700);
        window.show();
        // The navigation controller owns the tab bar and is constructed lazily.
        // Do not inspect it before establishing that owner.
        auto& ui = NavigationUiController::instance(window);

        auto* tabs = window.findChild<QTabBar*>(
            QStringLiteral("navigationTabs"));
        QVERIFY(tabs);
        QTRY_COMPARE(tabs->count(), 1);
        auto* tree = window.findChild<ChannelTree*>(
            QStringLiteral("channelList"));
        QVERIFY(tree);
        QTRY_VERIFY(tree->getCurrentPage()
            && tree->getCurrentPage()->getChannel().id == "channel");
        auto* log = tree->getCurrentPage()->findChild<ChatLogWidget*>(
            QStringLiteral("listWidget"));
        QVERIFY(log);
        QTRY_VERIFY(server.channelContextRequests.contains(targetId));

        // A body-only post lookup cannot assign ChannelPostSource an exact
        // position. The existing server-context adoption must materialize
        // the saved post at its relative viewport centre.
        qreal fraction = 0;
        QTRY_VERIFY(log->findPost(targetId)
            && log->source()->indexOfPost(targetId) >= 0
            && log->captureSemanticAnchorFraction(
                log->source()->indexOfPost(targetId),
                LongListWidget::SemanticAnchorPoint::ItemCenter,
                fraction)
            && qAbs(fraction) < 0.07);
        QVERIFY(!log->isAtEnd());

        ui.saveSession();
        const QJsonObject saved = QJsonDocument::fromJson(
            MLOptions::instance()->value<QByteArray>(sessionKey())).object();
        QCOMPARE(saved.value("tabs").toArray().at(0).toObject()
                     .value("anchor").toObject().value("post").toString(), targetId);
    }

    void sidebarSwitchAfterScrollingToNewestOverridesRestoredBookmark()
    {
        Server server;
        server.startupChannels = true;
        server.secondChannel = true;
        server.channelRootCount = 48;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/")
                                    .arg(server.serverPort()));

        const QString targetId = channelPostId(24);
        const QJsonObject destination {
            {"channel", "channel"}, {"root", ""},
            {"anchor", QJsonObject {
                {"post", targetId}, {"bottom", false}, {"fraction", 0.0}}}};
        MLOptions::instance()->setValue(sessionKey(),
            QJsonDocument(QJsonObject {
                {"version", 2}, {"tabs", QJsonArray {destination}},
                {"active_tab", destination}})
                .toJson(QJsonDocument::Compact));

        Backend backend;
        backend.getStorage().addUser(
            QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        QSystemTrayIcon tray;
        MainWindow window(nullptr, tray, backend);
        window.resize(1000, 700);
        window.show();
        auto& ui = NavigationUiController::instance(window);
        auto* tree = window.findChild<ChannelTree*>(
            QStringLiteral("channelList"));
        QVERIFY(tree);
        QTRY_VERIFY(tree->getCurrentPage()
            && tree->getCurrentPage()->getChannel().id == "channel");
        auto* firstArea = tree->getCurrentPage();
        auto* log = firstArea->findChild<ChatLogWidget*>(
            QStringLiteral("listWidget"));
        QVERIFY(log);

        qreal fraction = 0;
        QTRY_VERIFY(log->findPost(targetId)
            && log->captureSemanticAnchorFraction(
                log->source()->indexOfPost(targetId),
                LongListWidget::SemanticAnchorPoint::ItemCenter, fraction)
            && qAbs(fraction) < 0.07);
        QVERIFY(!log->isAtEnd());

        // A scrollbar action is user input: it must supersede a stale
        // restoration intent even if source materialization is still ongoing.
        log->verticalScrollBar()->triggerAction(
            QAbstractSlider::SliderToMaximum);
        QTRY_VERIFY(log->isAtEnd());
        QVERIFY(firstArea->property("sessionBookmark").toString().isEmpty());

        tree->openChannel(QStringLiteral("channel_other"));
        QTRY_VERIFY(tree->getCurrentPage()
            && tree->getCurrentPage()->getChannel().id == "channel_other");
        tree->openChannel(QStringLiteral("channel"));
        QTRY_COMPARE(tree->getCurrentPage(), firstArea);
        QTRY_VERIFY(log->isAtEnd());

        ui.saveSession();
        const QJsonObject saved = QJsonDocument::fromJson(
            MLOptions::instance()->value<QByteArray>(sessionKey())).object();
        const QJsonObject snapshot = saved.value("active_tab").toObject();
        QCOMPARE(snapshot.value("channel").toString(), QStringLiteral("channel"));
        const QJsonObject edgeAnchor = snapshot.value("anchor").toObject();
        // A cold live edge may have no concrete bottommost PostWidget yet.
        // In that case an absent bookmark means 'reopen newest', not the
        // stale middle-of-log bookmark from the previous session.
        QVERIFY(edgeAnchor.isEmpty()
            || (edgeAnchor.value("bottom").toBool()
                && edgeAnchor.value("post").toString() != targetId));
    }

    void semanticBookmarksSurviveRestartOnlyForOpenSurfaces()
    {
        Server server;
        server.startupChannels = true;
        // Use enough content above and below the saved target for the center
        // coordinate to be physically attainable at both Qt5 and Qt6 metrics.
        server.replyCount = 48;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));

        auto location = [](int thread, int reply, bool bottom, double fraction) {
            return QJsonObject {
                {"channel", "channel"},
                {"root", rootId(thread)},
                {"anchor", QJsonObject {
                    {"post", replyId(thread, reply)},
                    {"bottom", bottom},
                    {"fraction", fraction}}}};
        };
        const QJsonObject active = location(41, 24, false, -0.14);
        const QJsonObject inactive = location(42, 20, true, -0.03);
        QJsonObject detached = location(43, 24, false, 0.0);
        detached.insert("mode", "window");
        detached.insert("geometry", QString());

        const QString key = sessionKey();
        MLOptions::instance()->setValue(key,
            QJsonDocument(QJsonObject {
                {"version", 2},
                {"tabs", QJsonArray {active, inactive}},
                {"active_tab", active},
                {"threads", QJsonArray {detached}},
                {"focused_window", detached}})
                .toJson(QJsonDocument::Compact));

        Backend backend;
        backend.getStorage().addUser(
            QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        QSystemTrayIcon tray;
        MainWindow window(nullptr, tray, backend);
        window.show();
        auto& ui = NavigationUiController::instance(window);
        auto* tabs = window.findChild<QTabBar*>(QStringLiteral("navigationTabs"));
        QVERIFY(tabs);
        QTRY_COMPARE(tabs->count(), 2);
        QTRY_VERIFY(ui.findThread("channel", rootId(41)));
        QTRY_VERIFY(ui.findThread("channel", rootId(43)));
        // Detached windows are not owned by MainWindow. Guarantee their
        // destruction even when a QtTest assertion aborts this test early.
        QScopedPointer<ChatArea> detachedOwner(ui.findThread("channel", rootId(43)));
        // Inactive tabs are restored as semantic entries, not ChatAreas.
        QVERIFY(!ui.findThread("channel", rootId(42)));
        QTRY_VERIFY(server.postLookups.contains(replyId(41, 24)));
        QTRY_VERIFY(server.postLookups.contains(replyId(43, 24)));
        // Test the persisted measured anchor, not the temporary initial
        // viewport before the detached post has finished materializing.
        auto* detachedLog = ui.findThread("channel", rootId(43))
                                ->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QVERIFY(detachedLog);
        QTRY_VERIFY(detachedLog->findPost(replyId(43, 24)));

        // Restart must restore the same measured post/geometry rather than
        // merely serializing a plausible-looking JSON bookmark.
        auto* activeLog = ui.findThread("channel", rootId(41))
                              ->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QVERIFY(activeLog);
        qreal activeFraction = 0;
        const auto activeAnchored = [&] {
            if (!activeLog->findPost(replyId(41, 24))) return false;
            if (!activeLog->captureSemanticAnchorFraction(
                    activeLog->source()->indexOfPost(replyId(41, 24)),
                    LongListWidget::SemanticAnchorPoint::ItemCenter,
                    activeFraction)) return false;
            return qAbs(activeFraction + 0.14) < 0.07;
        };
        for (int elapsed = 0; elapsed < 15000 && !activeAnchored(); elapsed += 50) {
            QTest::qWait(50);
        }
        if (!activeAnchored()) {
            qWarning() << "Active thread anchor mismatch"
                       << "fraction=" << activeFraction
                       << "scroll=" << activeLog->verticalScrollBar()->value()
                       << "maximum=" << activeLog->verticalScrollBar()->maximum()
                       << "targetIndex=" << activeLog->source()->indexOfPost(replyId(41, 24));
        }
        QVERIFY(activeAnchored());
        qreal detachedFraction = 0;
        QTRY_VERIFY(detachedLog->captureSemanticAnchorFraction(
            detachedLog->source()->indexOfPost(replyId(43, 24)),
            LongListWidget::SemanticAnchorPoint::ItemCenter,
            detachedFraction)
            && qAbs(detachedFraction) < 0.07);

        ui.saveSession();
        QJsonObject saved = QJsonDocument::fromJson(
            MLOptions::instance()->value<QByteArray>(key)).object();
        QCOMPARE(saved.value("version").toInt(), 2);
        QCOMPARE(saved.value("tabs").toArray().size(), 2);
        ChatLogWidget::SemanticViewportBookmark visibleActive;
        ChatLogWidget::SemanticViewportBookmark visibleDetached;
        QVERIFY(activeLog->captureSemanticViewportBookmark(visibleActive));
        QVERIFY(detachedLog->captureSemanticViewportBookmark(visibleDetached));
        const QJsonObject savedActive = saved.value("tabs").toArray().at(0)
                                              .toObject().value("anchor").toObject();
        const auto matchesViewportOrPending = [](
            const QJsonObject& serialized,
            const ChatLogWidget::SemanticViewportBookmark& measured,
            const QString& requestedPost, qreal requestedFraction) {
            // While an asynchronous bookmark is settling, saveSession()
            // intentionally preserves the requested semantic anchor. Once
            // settled, it captures the centre post currently under the
            // viewport. Both represent the same visible position.
            const bool measuredAnchor =
                serialized.value("post").toString() == measured.postId
                && serialized.value("bottom").toBool()
                    == (measured.point == LongListWidget::SemanticAnchorPoint::ItemBottom)
                && qAbs(serialized.value("fraction").toDouble()
                        - measured.fraction) < 0.02;
            const bool pendingAnchor =
                serialized.value("post").toString() == requestedPost
                && !serialized.value("bottom").toBool()
                && qAbs(serialized.value("fraction").toDouble()
                        - requestedFraction) < 0.02;
            return measuredAnchor || pendingAnchor;
        };
        QVERIFY(matchesViewportOrPending(
            savedActive, visibleActive, replyId(41, 24), -0.14));
        // Inactive tabs preserve their original semantic bookmark verbatim.
        QCOMPARE(saved.value("tabs").toArray().at(1).toObject()
                     .value("anchor").toObject().value("post").toString(),
                 replyId(42, 20));
        QCOMPARE(saved.value("threads").toArray().size(), 1);
        const QJsonObject savedDetached = saved.value("threads").toArray().at(0)
                                                .toObject().value("anchor").toObject();
        QVERIFY(matchesViewportOrPending(
            savedDetached, visibleDetached, replyId(43, 24), 0.0));

        // Closing a tab removes its bookmark too; the snapshot must contain
        // only still-open presentation surfaces.
        QTest::mouseClick(tabs, Qt::MiddleButton, Qt::NoModifier,
                          tabs->tabRect(1).center());
        QTRY_COMPARE(tabs->count(), 1);
        ui.saveSession();
        saved = QJsonDocument::fromJson(
            MLOptions::instance()->value<QByteArray>(key)).object();
        QCOMPARE(saved.value("tabs").toArray().size(), 1);
        QVERIFY(activeLog->captureSemanticViewportBookmark(visibleActive));
        QVERIFY(matchesViewportOrPending(
            saved.value("tabs").toArray().at(0).toObject()
                 .value("anchor").toObject(),
            visibleActive, replyId(41, 24), -0.14));
    }

    void sessionDoesNotRestoreLegacyViewportBookmark()
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
        // A saved legacy post bookmark must not trigger random-seek traffic
        // or override the newest viewport at application launch.
        QTRY_VERIFY(log->isAtEnd());
        QVERIFY(!server.postLookups.contains(replyId(205, 365)));
        ui.saveSession();
        const auto saved = QJsonDocument::fromJson(
            MLOptions::instance()->value<QByteArray>(sessionKey())).object();
        QVERIFY(!saved.value("tabs").toArray().at(0).toObject().contains("post"));
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

    void dockedThreadCoexistsWithActiveThreadTab()
    {
        Server server;
        server.startupChannels = true;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        MLOptions::instance()->setValue(sessionKey(), QByteArray());

        Backend backend;
        auto& storage = backend.getStorage();
        storage.addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        QSystemTrayIcon tray;
        MainWindow window(nullptr, tray, backend);
        window.resize(1100, 700);
        window.show();

        QTRY_VERIFY(storage.getChannelById(QStringLiteral("channel")));
        QTRY_VERIFY(storage.getTeamById(QStringLiteral("team")));
        QTRY_VERIFY(SidebarService::instance(backend).teamState(QStringLiteral("team")));
        BackendChannel* channel = storage.getChannelById(QStringLiteral("channel"));
        QVERIFY(channel);
        channel->addPost(post(110));

        auto* team = storage.getTeamById(QStringLiteral("team"));
        QVERIFY(team);
        BackendChannel* other = storage.addTeamChannel(
            *team, QJsonObject {{"id", "other-channel"}, {"type", "O"},
                               {"display_name", "Other chat"}});
        QVERIFY(other);
        QJsonObject otherRoot = post(111);
        otherRoot.insert(QStringLiteral("channel_id"), other->id);
        otherRoot.insert(QStringLiteral("reply_count"), 0);
        otherRoot.insert(QStringLiteral("last_reply_at"),
                         otherRoot.value(QStringLiteral("create_at")));
        other->addPost(otherRoot);

        auto& navigationUi = NavigationUiController::instance(window);
        auto& navigation = AppNavigationService::instance(backend);
        navigation.openChannel(channel->id);
        QTRY_VERIFY(window.findChild<ChannelTree*>(QStringLiteral("channelList"))->getCurrentPage());

        navigation.openThreadInTab(channel->id, rootId(110));
        QTRY_VERIFY(navigationUi.findThread(channel->id, rootId(110)));
        ChatArea* tabbed = navigationUi.findThread(channel->id, rootId(110));
        QVERIFY(tabbed->property("threadTabbed").toBool());
        auto* surfaceStack = window.findChild<QStackedWidget*>(
            QStringLiteral("navigationSurfaceStack"));
        auto* threadStack = window.findChild<QStackedWidget*>(QStringLiteral("threadStack"));
        auto* tabs = window.findChild<QTabBar*>(QStringLiteral("navigationTabs"));
        QVERIFY(surfaceStack);
        QVERIFY(threadStack);
        QVERIFY(tabs);
        QCOMPARE(surfaceStack->currentWidget(), static_cast<QWidget*>(tabbed));
        const int tabCount = tabs->count();

        // This is the path used by an ordinary Following/Attention activation.
        // The central thread tab belongs to channel A; a thread from channel B
        // must be allowed to coexist in the independent right-hand pane.
        navigation.openThread(other->id, rootId(111));
        QTRY_VERIFY(navigationUi.findThread(other->id, rootId(111)));
        ChatArea* docked = navigationUi.findThread(other->id, rootId(111));
        QCOMPARE(tabs->count(), tabCount);
        QCOMPARE(surfaceStack->currentWidget(), static_cast<QWidget*>(tabbed));
        QTRY_VERIFY(!threadStack->isHidden());
        QCOMPARE(threadStack->currentWidget(), static_cast<QWidget*>(docked));
        QVERIFY(tabbed->isVisible());
        QVERIFY(docked->isVisible());
        QVERIFY(!docked->property("threadTabbed").toBool());

        // Re-activating the same thread must reuse the existing pane rather than
        // materialize a second ChatArea or disturb the central thread tab.
        navigation.openThread(other->id, rootId(111));
        QCOMPARE(navigationUi.findThread(other->id, rootId(111)), docked);
        QCOMPARE(threadStack->currentWidget(), static_cast<QWidget*>(docked));
        QCOMPARE(surfaceStack->currentWidget(), static_cast<QWidget*>(tabbed));
    }

    void scrollingUpFromLiveEdgeRestoresMeasuredPositionAfterTabSwitch()
    {
        Server server;
        server.replyCount = 24;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        auto& storage = backend.getStorage();
        storage.addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* team = storage.addTeam(QJsonObject {{"id", "team"}, {"name", "team"}});
        auto* channel = storage.addGroupChannel(
            QJsonObject {{"id", "channel"}, {"type", "G"},
                         {"display_name", "Test chat"}});
        QVERIFY(channel);
        QJsonObject root = post(41);
        root.insert("reply_count", server.replyCount);
        root.insert("last_reply_at", 1000000000000LL + 41000 + server.replyCount);
        channel->addPost(root);
        // This fixture tests geometry restoration, not sparse cursor seek.
        // Give both presentations the same complete, resident thread.
        for (int reply = 1; reply <= server.replyCount; ++reply) {
            channel->addPost(post(41, reply));
        }

        QSystemTrayIcon tray;
        MainWindow window(nullptr, tray, backend);
        window.findChild<ChannelTree*>(QStringLiteral("channelList"))->addTeam(backend, *team);
        auto& ui = NavigationUiController::instance(window);
        window.resize(1000, 700);
        window.show();
        auto& navigation = AppNavigationService::instance(backend);
        navigation.openThreadInTab(channel->id, rootId(41));
        QTRY_VERIFY(ui.findThread(channel->id, rootId(41)));
        auto* first = ui.findThread(channel->id, rootId(41));
        auto* log = first->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QVERIFY(log);
        QTRY_COMPARE(log->itemCount(), 25);
        auto* bar = log->verticalScrollBar();
        QVERIFY(bar);
        QTRY_VERIFY(bar->maximum() > 400);
        QTRY_VERIFY(log->findPost(replyId(41, 24)));
        QPointer<ChatArea> retired = first;
        QTest::qWait(120); // Drain initial deferred newest-position timers.
        log->scrollToEnd();
        QTRY_VERIFY(log->isAtEnd());

        // This gesture cancels the live-edge intent. The same physical
        // timeline must now be captured as a post-center anchor.
        // Read from a stable middle viewport with room on both sides.
        // Explicit scrollbar ownership must win over activation's weak tail
        // positioning, just as a real user scroll does.
        bar->triggerAction(QAbstractSlider::SliderSingleStepSub);
        bar->setValue(bar->maximum() / 2);
        QTRY_VERIFY(bar->value() > 0 && bar->value() < bar->maximum());
        QTRY_VERIFY(!log->isAtEnd());
        ChatLogWidget::SemanticViewportBookmark before;
        QTRY_VERIFY(log->captureSemanticViewportBookmark(before));
        QVERIFY(before.point == LongListWidget::SemanticAnchorPoint::ItemCenter);

        navigation.openThreadInTab(channel->id, rootId(42));
        QTRY_VERIFY(ui.findThread(channel->id, rootId(42)));
        // Removing the widget from its presentation owner must make it
        // undiscoverable immediately, even if Qt defers destruction.
        QVERIFY(!ui.findThread(channel->id, rootId(41)));
        QTRY_VERIFY(retired.isNull());

        // Exercise the user's actual tab-bar click path, not a new semantic
        // navigation request through AppNavigationService.
        auto* tabs = window.findChild<QTabBar*>(QStringLiteral("navigationTabs"));
        QVERIFY(tabs);
        QTRY_COMPARE(tabs->count(), 2);
        tabs->setCurrentIndex(0);
        QTRY_VERIFY(ui.findThread(channel->id, rootId(41)));
        auto* reopened = ui.findThread(channel->id, rootId(41));
        auto* restoredLog = reopened->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QVERIFY(restoredLog);
        QTRY_VERIFY(restoredLog->findPost(before.postId));
        QTRY_VERIFY(!restoredLog->isAtEnd());

        // A different post may cover the viewport centre after reflow.
        // Measure the saved identity itself rather than selecting a new
        // centre post, which can change at row boundaries.
        const int anchorIndex = restoredLog->source()->indexOfPost(before.postId);
        QVERIFY(anchorIndex >= 0);
        qreal restoredFraction = 0;
        QTRY_VERIFY(restoredLog->findPost(before.postId)
            && restoredLog->captureSemanticAnchorFraction(
                restoredLog->source()->indexOfPost(before.postId),
                LongListWidget::SemanticAnchorPoint::ItemCenter,
                restoredFraction)
            && qAbs(restoredFraction - before.fraction) < 0.06);
    }

    void backHistoryRestoresMeasuredPostCenter()
    {
        Server server;
        server.replyCount = 24;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        NetworkRequest::setHost(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
        Backend backend;
        auto& storage = backend.getStorage();
        storage.addUser(QJsonObject {{"id", "user"}, {"username", "tester"}}, true);
        auto* team = storage.addTeam(QJsonObject {{"id", "team"}, {"name", "team"}});
        auto* channel = storage.addGroupChannel(
            QJsonObject {{"id", "channel"}, {"type", "G"},
                         {"display_name", "Test chat"}});
        QVERIFY(channel);
        QJsonObject root = post(41);
        root.insert("reply_count", server.replyCount);
        root.insert("last_reply_at", 1000000000000LL + 41000 + server.replyCount);
        channel->addPost(root);
        // This fixture tests geometry restoration, not sparse cursor seek.
        // Give both presentations the same complete, resident thread.
        for (int reply = 1; reply <= server.replyCount; ++reply) {
            channel->addPost(post(41, reply));
        }

        QSystemTrayIcon tray;
        MainWindow window(nullptr, tray, backend);
        window.findChild<ChannelTree*>(QStringLiteral("channelList"))->addTeam(backend, *team);
        auto& ui = NavigationUiController::instance(window);
        window.resize(1000, 700);
        window.show();

        auto& navigation = AppNavigationService::instance(backend);
        navigation.openThreadInTab(channel->id, rootId(41));
        QTRY_VERIFY(ui.findThread(channel->id, rootId(41)));
        auto* log = ui.findThread(channel->id, rootId(41))
                        ->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QVERIFY(log);
        QTRY_COMPARE(log->itemCount(), 25);
        QTRY_VERIFY(log->verticalScrollBar()->maximum() > 400);
        QTRY_VERIFY(log->findPost(replyId(41, 24)));
        QTest::qWait(120); // Drain initial deferred newest-position timers.
        log->scrollToEnd();
        QTRY_VERIFY(log->isAtEnd());
        auto* bar = log->verticalScrollBar();
        // Read from a stable middle viewport with room on both sides.
        // Explicit scrollbar ownership must win over activation's weak tail
        // positioning, just as a real user scroll does.
        bar->triggerAction(QAbstractSlider::SliderSingleStepSub);
        bar->setValue(bar->maximum() / 2);
        QTRY_VERIFY(bar->value() > 0 && bar->value() < bar->maximum());
        QTRY_VERIFY(!log->isAtEnd());

        ChatLogWidget::SemanticViewportBookmark before;
        QTRY_VERIFY(log->captureSemanticViewportBookmark(before));
        QVERIFY(before.point == LongListWidget::SemanticAnchorPoint::ItemCenter);

        navigation.openThreadInTab(channel->id, rootId(42));
        QTRY_VERIFY(ui.findThread(channel->id, rootId(42)));
        auto* back = window.findChild<QToolButton*>(
            QStringLiteral("navigationBackButton"));
        QVERIFY(back);
        QTRY_VERIFY(back->isEnabled());
        back->click();

        QTRY_VERIFY(ui.findThread(channel->id, rootId(41)));
        auto* restored = ui.findThread(channel->id, rootId(41))
                             ->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QVERIFY(restored);
        qreal actualFraction = 0;
        QTRY_VERIFY(restored->findPost(before.postId)
            && restored->captureSemanticAnchorFraction(
                restored->source()->indexOfPost(before.postId),
                LongListWidget::SemanticAnchorPoint::ItemCenter,
                actualFraction)
            && qAbs(actualFraction - before.fraction) < 0.06);
        QVERIFY(!restored->isAtEnd());
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
        auto* tabs = window.findChild<QTabBar*>(QStringLiteral("navigationTabs"));
        QVERIFY(tabs);
        const int initialTabs = tabs->count();
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

            // Inactive tabs retain their semantic destination, not a hidden
            // ChatArea, its post source, or any pending view state.
            for (int previous = 1; previous < thread; ++previous) {
                QTRY_VERIFY(!ui.findThread(channel->id, rootId(previous)));
            }
        }
        QCOMPARE(tabs->count(), initialTabs + 8);
        QCOMPARE(server.postLookups.size(), 7);
        QVERIFY(!server.postLookups.contains(rootId(1)));

        // An inactive tab must reconstruct its view on activation without
        // creating another tab entry or losing its saved destination.
        navigation.openThreadInTab(channel->id, rootId(1));
        QTRY_VERIFY(ui.findThread(channel->id, rootId(1)));
        auto* reopened = ui.findThread(channel->id, rootId(1));
        QTRY_VERIFY(reopened->findChild<ChatLogWidget*>(QStringLiteral("listWidget"))
                        ->findPost(replyId(1, 3)));
        QVERIFY(reopened->isVisible());
        QCOMPARE(tabs->count(), initialTabs + 8);
        QTRY_VERIFY(!ui.findThread(channel->id, rootId(8)));

        // Rapid tab switches while root requests are in flight must not keep
        // invisible thread views alive or require their replies to arrive.
        server.delayMs = 50;
        for (int thread = 21; thread <= 28; ++thread) {
            navigation.openThreadInTab(channel->id, rootId(thread));
            QTRY_VERIFY(ui.findThread(channel->id, rootId(thread)));
            for (int previous = 21; previous < thread; ++previous) {
                QTRY_VERIFY(!ui.findThread(channel->id, rootId(previous)));
            }
        }
        QCOMPARE(tabs->count(), initialTabs + 16);
        auto* latest = ui.findThread(channel->id, rootId(28));
        QVERIFY(latest);
        auto* latestList = latest->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QVERIFY(latestList);
        QTRY_COMPARE(latestList->itemCount(), 4);
        QTRY_VERIFY(latestList->findPost(replyId(28, 3)));
        QVERIFY(latest->isVisible());
        QTRY_VERIFY(server.postLookups.contains(rootId(28)));

        // A previously inactive cold thread only needs to materialize when
        // selected again; old, delayed responses cannot resurrect its view.
        navigation.openThreadInTab(channel->id, rootId(21));
        QTRY_VERIFY(ui.findThread(channel->id, rootId(21)));
        auto* revisited = ui.findThread(channel->id, rootId(21));
        auto* revisitedList = revisited->findChild<ChatLogWidget*>(QStringLiteral("listWidget"));
        QVERIFY(revisitedList);
        QTRY_COMPARE(revisitedList->itemCount(), 4);
        QTRY_VERIFY(revisitedList->findPost(replyId(21, 3)));
        QVERIFY(revisited->isVisible());
        QCOMPARE(tabs->count(), initialTabs + 16);
        QTRY_VERIFY(!ui.findThread(channel->id, rootId(28)));
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
    QCoreApplication::setOrganizationName(QStringLiteral("MatterLeastTest"));
    QCoreApplication::setApplicationName(QStringLiteral("thread-tabs-integration-test"));
    QStandardPaths::setTestModeEnabled(true);
    QLoggingCategory::setFilterRules(QStringLiteral("*.debug=false\n"));
    ThreadTabsIntegrationTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "ThreadTabsIntegrationTest.moc"