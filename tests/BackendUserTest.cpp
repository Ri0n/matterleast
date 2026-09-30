#include <QtTest>

#include <QJsonObject>

#include "backend/RecentMentionsPolicy.h"
#include "backend/types/BackendUser.h"

using namespace Mattermost;

namespace {

QJsonObject userJson(qulonglong pictureVersion, qulonglong updateAt,
                     const QString& firstName = QStringLiteral("Alice"))
{
    return QJsonObject {
        {QStringLiteral("id"), QStringLiteral("user-id")},
        {QStringLiteral("create_at"), 1},
        {QStringLiteral("update_at"), static_cast<qint64>(updateAt)},
        {QStringLiteral("delete_at"), 0},
        {QStringLiteral("username"), QStringLiteral("alice")},
        {QStringLiteral("email"), QStringLiteral("alice@example.invalid")},
        {QStringLiteral("first_name"), firstName},
        {QStringLiteral("last_name"), QStringLiteral("Example")},
        {QStringLiteral("last_picture_update"), static_cast<qint64>(pictureVersion)},
        {QStringLiteral("notify_props"), QJsonObject {}},
        {QStringLiteral("props"), QJsonObject {}},
        {QStringLiteral("timezone"), QJsonObject {}},
    };
}

} // namespace

class BackendUserTest : public QObject
{
    Q_OBJECT

private slots:
    void parsesPictureVersion()
    {
        BackendUser user(userJson(1234, 100));

        QCOMPARE(user.last_picture_update, uint64_t(1234));
        QCOMPARE(user.avatar_picture_update, uint64_t(0));
    }

    void profileUpdateChangesPictureVersionButNotLoadedAvatarVersion()
    {
        BackendUser user(userJson(1234, 100));
        user.avatar_picture_update = 1234;

        BackendUser updated(userJson(5678, 200, QStringLiteral("Alicia")));
        QString changes;
        user.updateFrom(updated, changes);

        QCOMPARE(user.last_picture_update, uint64_t(5678));
        QCOMPARE(user.avatar_picture_update, uint64_t(1234));
        QCOMPARE(user.update_at, uint64_t(200));
        QCOMPARE(user.first_name, QStringLiteral("Alicia"));
        QVERIFY(changes.contains(QStringLiteral("last_picture_update")));
    }

    void parsesStringAndBooleanNotificationFlags()
    {
        QJsonObject json = userJson(1234, 100);
        json.insert(QStringLiteral("notify_props"), QJsonObject {
            {QStringLiteral("channel"), QStringLiteral("true")},
            {QStringLiteral("desktop_sound"), QStringLiteral("TRUE")},
            {QStringLiteral("email"), true},
            {QStringLiteral("first_name"), QStringLiteral("true")},
            {QStringLiteral("mention_keys"), QStringLiteral("alpha,,beta")},
        });

        const BackendUser user(json);
        QVERIFY(user.notify_preps.channel);
        QVERIFY(user.notify_preps.desktop_sound);
        QVERIFY(user.notify_preps.email);
        QVERIFY(user.notify_preps.first_name);
        QCOMPARE(user.notify_preps.mention_keys,
                 QStringList({QStringLiteral("alpha"), QStringLiteral("beta")}));
    }

    void recentMentionsUseQuotedPersonalMentionKeys()
    {
        QJsonObject json = userJson(1234, 100);
        json.insert(QStringLiteral("notify_props"), QJsonObject {
            {QStringLiteral("first_name"), QStringLiteral("true")},
            {QStringLiteral("mention_keys"),
             QStringLiteral("custom-hyphen,@channel,@all,@here,@alice")},
        });

        const BackendUser user(json);
        QCOMPARE(RecentMentionsPolicy::searchTerms(user),
                 QStringLiteral("\"custom-hyphen\" \"@alice\" \"Alice\""));
    }
};

QTEST_MAIN(BackendUserTest)

#include "BackendUserTest.moc"
