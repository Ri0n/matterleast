#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/WebSocketEventHandler.h"
#include "backend/events/PostEvent.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendTeam.h"

using namespace Mattermost;

class WebSocketPostedReplayTest final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void materializedHistoryDoesNotBecomeNewPostOnReplay()
    {
        Backend backend;
        Storage& storage = backend.getStorage();
        BackendTeam* team = storage.addTeam(QJsonObject{
            {QStringLiteral("id"), QStringLiteral("team")},
            {QStringLiteral("name"), QStringLiteral("team")}
        });
        QVERIFY(team);
        BackendChannel* channel = storage.addTeamChannel(*team, QJsonObject{
            {QStringLiteral("id"), QStringLiteral("channel")},
            {QStringLiteral("team_id"), QStringLiteral("team")},
            {QStringLiteral("name"), QStringLiteral("general")},
            {QStringLiteral("display_name"), QStringLiteral("General")},
            {QStringLiteral("type"), QStringLiteral("O")}
        });
        QVERIFY(channel);

        // This post was already loaded through an HTTP history request.
        const QJsonObject post{
            {QStringLiteral("id"), QStringLiteral("known-post")},
            {QStringLiteral("channel_id"), QStringLiteral("channel")},
            {QStringLiteral("user_id"), QStringLiteral("other-user")},
            {QStringLiteral("message"), QStringLiteral("old message")},
            {QStringLiteral("create_at"), qint64(1700000000000LL)}
        };
        BackendPost* const existing = channel->addPost(post);
        QVERIFY(existing);

        int backendNotifications = 0;
        int channelNotifications = 0;
        QObject::connect(&backend, &Backend::onNewPost, &backend,
                         [&](BackendChannel&, const BackendPost&) {
            ++backendNotifications;
        });
        QObject::connect(channel, &BackendChannel::onNewPost, &backend,
                         [&](BackendPost&) {
            ++channelNotifications;
        });

        const PostEvent replay(
            QJsonObject{{QStringLiteral("post"),
                         QString::fromUtf8(QJsonDocument(post).toJson(QJsonDocument::Compact))}},
            QJsonObject{{QStringLiteral("channel_id"), QStringLiteral("channel")}});
        WebSocketEventHandler handler(backend);
        handler.handleEvent(replay);
        handler.handleEvent(replay);

        // Neither replay is a new post. In particular, MainWindow's
        // desktop notifications must not be triggered by onNewPost.
        QCOMPARE(channel->findPostById(QStringLiteral("known-post")), existing);
        QCOMPARE(backendNotifications, 0);
        QCOMPARE(channelNotifications, 0);
    }
};

QTEST_MAIN(WebSocketPostedReplayTest)
#include "WebSocketPostedReplayTest.moc"
