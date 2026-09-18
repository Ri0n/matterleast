#include <QtTest>

#include "chat-area/PostFilterPolicy.h"

using namespace Mattermost;

class PostFilterPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void routineMembershipTypesAreHidden_data()
    {
        QTest::addColumn<QString>("type");

        QTest::newRow("legacy-join-leave") << QStringLiteral("system_join_leave");
        QTest::newRow("join-channel") << QStringLiteral("system_join_channel");
        QTest::newRow("leave-channel") << QStringLiteral("system_leave_channel");
        QTest::newRow("legacy-add-remove") << QStringLiteral("system_add_remove");
        QTest::newRow("add-channel") << QStringLiteral("system_add_to_channel");
        QTest::newRow("remove-channel") << QStringLiteral("system_remove_from_channel");
        QTest::newRow("join-team") << QStringLiteral("system_join_team");
        QTest::newRow("leave-team") << QStringLiteral("system_leave_team");
        QTest::newRow("add-team") << QStringLiteral("system_add_to_team");
        QTest::newRow("remove-team") << QStringLiteral("system_remove_from_team");
    }

    void routineMembershipTypesAreHidden()
    {
        QFETCH(QString, type);
        QVERIFY(!shouldShowMainTimelinePost(
            type, QJsonObject {}, QStringLiteral("current")));
    }

    void currentUserMembershipEventsRemainVisible_data()
    {
        QTest::addColumn<QString>("field");

        QTest::newRow("username") << QStringLiteral("username");
        QTest::newRow("added-user") << QStringLiteral("addedUsername");
        QTest::newRow("removed-user") << QStringLiteral("removedUsername");
    }

    void currentUserMembershipEventsRemainVisible()
    {
        QFETCH(QString, field);
        QJsonObject props {{field, QStringLiteral("current")}};

        QVERIFY(shouldShowMainTimelinePost(
            QStringLiteral("system_join_channel"),
            props,
            QStringLiteral("current")));
    }

    void meaningfulSystemPostsRemainVisible()
    {
        const QString current = QStringLiteral("current");

        QVERIFY(shouldShowMainTimelinePost(
            QStringLiteral("system_header_change"), QJsonObject {}, current));
        QVERIFY(shouldShowMainTimelinePost(
            QStringLiteral("system_purpose_change"), QJsonObject {}, current));
        QVERIFY(shouldShowMainTimelinePost(
            QStringLiteral("system_channel_deleted"), QJsonObject {}, current));
        QVERIFY(shouldShowMainTimelinePost(
            QStringLiteral("system_guest_join_channel"), QJsonObject {}, current));
        QVERIFY(shouldShowMainTimelinePost(
            QStringLiteral("system_add_guest_to_chan"), QJsonObject {}, current));
    }
};

QTEST_MAIN(PostFilterPolicyTest)
#include "PostFilterPolicyTest.moc"
