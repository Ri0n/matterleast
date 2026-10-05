#include <QtTest>

#include "navigation/MattermostUrlRouter.h"
#include "navigation/NavigationRequestGate.h"

using namespace Mattermost;

class NavigationRequestGateTest : public QObject
{
    Q_OBJECT

private slots:
    void newerRequestSupersedesOlderCallback()
    {
        NavigationRequestGate gate;
        const quint64 first = gate.begin();
        QVERIFY(gate.isCurrent(first));

        const quint64 second = gate.begin();
        QVERIFY(second > first);
        QVERIFY(!gate.isCurrent(first));
        QVERIFY(gate.isCurrent(second));
    }

    void recognizesMattermostWebRoutes()
    {
        const QUrl serverUrl(QStringLiteral("https://chat.example.invalid/"));

        const MattermostUrlRoute channel = MattermostUrlRouter::classify(
            QUrl(QStringLiteral(
                "https://chat.example.invalid/engineering/channels/town-square")),
            serverUrl);
        QVERIFY(channel.kind == MattermostUrlRoute::Kind::Channel);
        QCOMPARE(channel.teamName, QStringLiteral("engineering"));
        QCOMPARE(channel.target, QStringLiteral("town-square"));

        const MattermostUrlRoute permalink = MattermostUrlRouter::classify(
            QUrl(QStringLiteral(
                "https://chat.example.invalid/engineering/pl/post-id?from=search#reply")),
            serverUrl);
        QVERIFY(permalink.kind == MattermostUrlRoute::Kind::Post);
        QCOMPARE(permalink.target, QStringLiteral("post-id"));

        const MattermostUrlRoute direct = MattermostUrlRouter::classify(
            QUrl(QStringLiteral("/engineering/messages/@alice")), serverUrl);
        QVERIFY(direct.kind == MattermostUrlRoute::Kind::DirectMessage);
        QCOMPARE(direct.target, QStringLiteral("@alice"));

        const MattermostUrlRoute group = MattermostUrlRouter::classify(
            QUrl(QStringLiteral("/engineering/messages/group-token")), serverUrl);
        QVERIFY(group.kind == MattermostUrlRoute::Kind::GroupMessage);
        QCOMPARE(group.target, QStringLiteral("group-token"));
    }

    void buildsCanonicalConversationPaths()
    {
        QCOMPARE(MattermostUrlRouter::conversationPath(
                     MattermostUrlRoute::Kind::Channel,
                     QStringLiteral("engineering"),
                     QStringLiteral("town-square")),
                 QStringLiteral("/engineering/channels/town-square"));
        QCOMPARE(MattermostUrlRouter::conversationPath(
                     MattermostUrlRoute::Kind::DirectMessage,
                     QStringLiteral("engineering"),
                     QStringLiteral("alice")),
                 QStringLiteral("/engineering/messages/@alice"));
        QCOMPARE(MattermostUrlRouter::conversationPath(
                     MattermostUrlRoute::Kind::DirectMessage,
                     QStringLiteral("engineering"),
                     QStringLiteral("@alice")),
                 QStringLiteral("/engineering/messages/@alice"));
        QCOMPARE(MattermostUrlRouter::conversationPath(
                     MattermostUrlRoute::Kind::GroupMessage,
                     QStringLiteral("engineering"),
                     QStringLiteral("group-token")),
                 QStringLiteral("/engineering/messages/group-token"));
    }

    void requiresExactMattermostOrigin()
    {
        const QUrl serverUrl(QStringLiteral("https://chat.example.invalid/"));

        QVERIFY(MattermostUrlRouter::classify(
                    QUrl(QStringLiteral(
                        "https://chat.example.invalid:443/engineering/pl/post-id")),
                    serverUrl).kind
                == MattermostUrlRoute::Kind::Post);
        QVERIFY(MattermostUrlRouter::classify(
                    QUrl(QStringLiteral(
                        "http://chat.example.invalid/engineering/pl/post-id")),
                    serverUrl).kind
                == MattermostUrlRoute::Kind::External);
        QVERIFY(MattermostUrlRouter::classify(
                    QUrl(QStringLiteral(
                        "https://chat.example.invalid:8443/engineering/pl/post-id")),
                    serverUrl).kind
                == MattermostUrlRoute::Kind::External);
        QVERIFY(MattermostUrlRouter::classify(
                    QUrl(QStringLiteral(
                        "https://chat.example.invalid.example.net/engineering/pl/post-id")),
                    serverUrl).kind
                == MattermostUrlRoute::Kind::External);
        QVERIFY(MattermostUrlRouter::classify(
                    QUrl(QStringLiteral(
                        "https://other-chat.example.invalid/engineering/pl/post-id")),
                    serverUrl).kind
                == MattermostUrlRoute::Kind::External);
        QVERIFY(MattermostUrlRouter::classify(
                    QUrl(QStringLiteral(
                        "custom://chat.example.invalid/engineering/pl/post-id")),
                    QUrl(QStringLiteral("custom://chat.example.invalid/"))).kind
                == MattermostUrlRoute::Kind::External);
    }

    void respectsMattermostBasePath()
    {
        const QUrl serverUrl(QStringLiteral("https://example.com/mattermost/"));

        const MattermostUrlRoute inside = MattermostUrlRouter::classify(
            QUrl(QStringLiteral(
                "https://example.com/mattermost/team/channels/town-square")),
            serverUrl);
        QVERIFY(inside.kind == MattermostUrlRoute::Kind::Channel);
        QCOMPARE(inside.teamName, QStringLiteral("team"));
        QCOMPARE(inside.target, QStringLiteral("town-square"));

        QVERIFY(MattermostUrlRouter::classify(
                    QUrl(QStringLiteral("https://example.com/team/channels/town-square")),
                    serverUrl).kind
                == MattermostUrlRoute::Kind::External);
        QVERIFY(MattermostUrlRouter::classify(
                    QUrl(QStringLiteral(
                        "https://example.com/mattermost-extra/team/channels/town-square")),
                    serverUrl).kind
                == MattermostUrlRoute::Kind::External);
    }
};

QTEST_APPLESS_MAIN(NavigationRequestGateTest)
#include "NavigationRequestGateTest.moc"
