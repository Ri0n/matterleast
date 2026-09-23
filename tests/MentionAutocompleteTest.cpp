#include <QJsonObject>
#include <QtTest>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendChannelMember.h"
#include "backend/types/BackendUser.h"
#include "chat-area/outgoing-post/MentionAutocomplete.h"

using namespace Mattermost;

namespace {

QJsonObject userJson(const QString& id,
                     const QString& username,
                     const QString& firstName = {},
                     const QString& lastName = {},
                     const QString& nickname = {})
{
    return QJsonObject {
        {QStringLiteral("id"), id},
        {QStringLiteral("create_at"), 1},
        {QStringLiteral("update_at"), 1},
        {QStringLiteral("delete_at"), 0},
        {QStringLiteral("username"), username},
        {QStringLiteral("email"), username + QStringLiteral("@example.invalid")},
        {QStringLiteral("first_name"), firstName},
        {QStringLiteral("last_name"), lastName},
        {QStringLiteral("nickname"), nickname},
        {QStringLiteral("notify_props"), QJsonObject {}},
        {QStringLiteral("props"), QJsonObject {}},
        {QStringLiteral("timezone"), QJsonObject {}},
    };
}

void addChannelMember(Storage& storage,
                      BackendChannel& channel,
                      const QString& userId)
{
    channel.members.insert(
        userId,
        BackendChannelMember(
            storage,
            QJsonObject {
                {QStringLiteral("user_id"), userId},
                {QStringLiteral("last_viewed_at"), 0},
                {QStringLiteral("msg_count"), 0},
                {QStringLiteral("mention_count"), 0},
                {QStringLiteral("scheme_admin"), false},
            }));
}

} // namespace

class MentionAutocompleteTest : public QObject
{
    Q_OBJECT

private slots:
    void matchingUsesMattermostStylePrefixes()
    {
        BackendUser user(userJson(
            QStringLiteral("user-id"),
            QStringLiteral("rick.target-user"),
            QStringLiteral("Alice"),
            QStringLiteral("Example Person"),
            QStringLiteral("The Boss")));

        QVERIFY(mentionUserMatchesPrefix(user, QStringLiteral("rick")));
        QVERIFY(mentionUserMatchesPrefix(user, QStringLiteral("target")));
        QVERIFY(mentionUserMatchesPrefix(user, QStringLiteral("user")));
        QVERIFY(mentionUserMatchesPrefix(user, QStringLiteral("ali")));
        QVERIFY(mentionUserMatchesPrefix(user, QStringLiteral("exa")));
        QVERIFY(mentionUserMatchesPrefix(user, QStringLiteral("person")));
        QVERIFY(mentionUserMatchesPrefix(user, QStringLiteral("boss")));
        QVERIFY(!mentionUserMatchesPrefix(user, QStringLiteral("arget")));
        QVERIFY(!mentionUserMatchesPrefix(user, QStringLiteral("lice")));
    }

    void localCandidatesAreBoundedToChannelMembers()
    {
        Backend backend;
        Storage& storage = backend.getStorage();
        BackendChannel channel(
            storage,
            QJsonObject {
                {QStringLiteral("id"), QStringLiteral("channel-id")},
                {QStringLiteral("type"), QStringLiteral("O")},
            });

        for (int index = 0; index < 40; ++index) {
            const QString id = QStringLiteral("member-%1").arg(index);
            BackendUser* user = storage.addUser(userJson(
                id,
                QStringLiteral("alex.member.%1").arg(index),
                QStringLiteral("Alex"),
                QStringLiteral("Member")));
            QVERIFY(user);
            addChannelMember(storage, channel, id);
        }

        // Populate the global cache with many matching users that are not in the
        // channel. The composer-local path must not enumerate or return them.
        for (int index = 0; index < 1000; ++index) {
            BackendUser* user = storage.addUser(userJson(
                QStringLiteral("global-%1").arg(index),
                QStringLiteral("alex.global.%1").arg(index),
                QStringLiteral("Alex"),
                QStringLiteral("Global")));
            QVERIFY(user);
        }

        const QVector<const BackendUser*> users =
            localMentionUsers(channel, QStringLiteral("alex"), 25);

        QCOMPARE(users.size(), 25);
        for (const BackendUser* user : users) {
            QVERIFY(user);
            QVERIFY(channel.members.contains(user->id));
            QVERIFY(!user->id.startsWith(QStringLiteral("global-")));
        }
    }
};

QTEST_MAIN(MentionAutocompleteTest)
#include "MentionAutocompleteTest.moc"
