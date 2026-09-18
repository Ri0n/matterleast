#include <QJsonDocument>
#include <QtTest>

#include "backend/events/MultipleChannelsViewedEvent.h"
#include "backend/events/ThreadUpdatedEvent.h"

using namespace Mattermost;

class MultipleChannelsViewedEventTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesThreadUpdatedPayload()
    {
        const QJsonObject thread {
            {QStringLiteral("id"), QStringLiteral("thread-id")},
            {QStringLiteral("last_viewed_at"), 1234},
            {QStringLiteral("unread_replies"), 2},
        };
        const QJsonObject data {
            {QStringLiteral("thread"),
             QString::fromUtf8(QJsonDocument(thread).toJson(QJsonDocument::Compact))},
        };
        const QJsonObject broadcast {
            {QStringLiteral("user_id"), QStringLiteral("user-id")},
            {QStringLiteral("team_id"), QStringLiteral("team-id")},
        };

        const ThreadUpdatedEvent event(data, broadcast);
        QVERIFY(event.valid);
        QCOMPARE(event.userId, QStringLiteral("user-id"));
        QCOMPARE(event.teamId, QStringLiteral("team-id"));
        QCOMPARE(event.threadId, QStringLiteral("thread-id"));

        const ThreadUpdatedEvent invalid(
            QJsonObject {{QStringLiteral("thread"), QStringLiteral("{broken")}},
            broadcast);
        QVERIFY(!invalid.valid);
        QVERIFY(invalid.threadId.isEmpty());
    }

    void parsesAuthoritativeChannelTimes()
    {
        const QJsonObject data {
            {QStringLiteral("channel_times"),
             QJsonObject {
                 {QStringLiteral("channel-a"), 1789743703189.0},
                 {QStringLiteral("channel-b"), 1789743704000.0},
                 {QStringLiteral("channel-zero"), 0},
                 {QString(), 1789743705000.0},
             }},
        };
        const QJsonObject broadcast {
            {QStringLiteral("user_id"), QStringLiteral("user-id")},
        };

        const MultipleChannelsViewedEvent event(data, broadcast);

        QCOMPARE(event.userId, QStringLiteral("user-id"));
        QCOMPARE(event.channelTimes.size(), 3);
        QCOMPARE(event.channelTimes.value(QStringLiteral("channel-a")),
                 uint64_t(1789743703189ULL));
        QCOMPARE(event.channelTimes.value(QStringLiteral("channel-b")),
                 uint64_t(1789743704000ULL));
        QCOMPARE(event.channelTimes.value(QStringLiteral("channel-zero")),
                 uint64_t(0));
    }
};

QTEST_MAIN(MultipleChannelsViewedEventTest)

#include "MultipleChannelsViewedEventTest.moc"
