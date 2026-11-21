#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include "workspaces/WorkspaceLayout.h"

using Mattermost::WorkspaceLayout;

namespace {
WorkspaceLayout sampleLayout()
{
    WorkspaceLayout layout;
    layout.accounts = {
        {QStringLiteral("s1"), QStringLiteral("Serv1")},
        {QStringLiteral("s2"), QStringLiteral("Serv2")},
        {QStringLiteral("s3"), QStringLiteral("Serv3")},
        {QStringLiteral("s4"), QStringLiteral("Serv4")},
        {QStringLiteral("s5"), QStringLiteral("Serv5")}};
    layout.profiles = {
        {QStringLiteral("sim"), QStringLiteral("Simultaneous"),
         {QStringLiteral("s1"), QStringLiteral("s2"), QStringLiteral("s3")}},
        {QStringLiteral("solo4"), QStringLiteral("Serv4"), {QStringLiteral("s4")}},
        {QStringLiteral("solo5"), QStringLiteral("Serv5"), {QStringLiteral("s5")}}};
    layout.views = {
        {QStringLiteral("merged"), QStringLiteral("sim"),
         QStringLiteral("Merged"), {QStringLiteral("s1"), QStringLiteral("s2")}},
        {QStringLiteral("s3view"), QStringLiteral("sim"),
         QStringLiteral("Serv3"), {QStringLiteral("s3")}},
        {QStringLiteral("s4view"), QStringLiteral("solo4"),
         QStringLiteral("Serv4"), {QStringLiteral("s4")}},
        {QStringLiteral("s5view"), QStringLiteral("solo5"),
         QStringLiteral("Serv5"), {QStringLiteral("s5")}}};
    layout.activeViewId = QStringLiteral("merged");
    return layout;
}
} // namespace

class WorkspaceLayoutTest : public QObject
{
    Q_OBJECT

private slots:
    void viewSwitchKeepsAllProfileAccountsConnected()
    {
        WorkspaceLayout layout = sampleLayout();
        QVERIFY(layout.validate());
        const auto transition = layout.transitionTo(QStringLiteral("s3view"));
        QVERIFY(transition.has_value());
        QVERIFY(transition->disconnectAccountIds.isEmpty());
        QVERIFY(transition->connectAccountIds.isEmpty());
        QCOMPARE(transition->visibleAccountIds, QStringList({QStringLiteral("s3")}));
        QVERIFY(layout.selectView(QStringLiteral("s3view")));
        QCOMPARE(layout.activeViewId, QStringLiteral("s3view"));
    }

    void exclusiveProfileSwitchPlansDisconnectBeforeConnect()
    {
        WorkspaceLayout layout = sampleLayout();
        const auto transition = layout.transitionTo(QStringLiteral("s4view"));
        QVERIFY(transition.has_value());
        QCOMPARE(transition->disconnectAccountIds,
                 QStringList({QStringLiteral("s1"), QStringLiteral("s2"),
                              QStringLiteral("s3")}));
        QCOMPARE(transition->connectAccountIds,
                 QStringList({QStringLiteral("s4")}));
        QCOMPARE(transition->targetProfileId, QStringLiteral("solo4"));
        QVERIFY(layout.selectView(QStringLiteral("s4view")));
        const auto returnPlan = layout.transitionTo(QStringLiteral("merged"));
        QVERIFY(returnPlan.has_value());
        QCOMPARE(returnPlan->disconnectAccountIds, QStringList({QStringLiteral("s4")}));
        QCOMPARE(returnPlan->connectAccountIds,
                 QStringList({QStringLiteral("s1"), QStringLiteral("s2"),
                              QStringLiteral("s3")}));
    }

    void overlappingViewsShareConnections()
    {
        WorkspaceLayout layout = sampleLayout();
        layout.views.append({QStringLiteral("work"), QStringLiteral("sim"),
                             QStringLiteral("Work"),
                             {QStringLiteral("s2"), QStringLiteral("s3")}});
        QVERIFY(layout.validate());
        const auto transition = layout.transitionTo(QStringLiteral("work"));
        QVERIFY(transition.has_value());
        QVERIFY(transition->connectAccountIds.isEmpty());
        QVERIFY(transition->disconnectAccountIds.isEmpty());
        QCOMPARE(transition->visibleAccountIds,
                 QStringList({QStringLiteral("s2"), QStringLiteral("s3")}));
    }

    void overlappingProfilesReuseSharedAccount()
    {
        WorkspaceLayout layout = sampleLayout();
        layout.profiles[1].accountIds.prepend(QStringLiteral("s2"));
        layout.views[2].accountIds.prepend(QStringLiteral("s2"));
        QVERIFY(layout.validate());
        const auto transition = layout.transitionTo(QStringLiteral("s4view"));
        QVERIFY(transition.has_value());
        QCOMPARE(transition->disconnectAccountIds,
                 QStringList({QStringLiteral("s1"), QStringLiteral("s3")}));
        QCOMPARE(transition->connectAccountIds,
                 QStringList({QStringLiteral("s4")}));
    }

    void stableIdsSurviveRenameAndRoundTrip()
    {
        WorkspaceLayout layout = sampleLayout();
        layout.accounts[0].name = QStringLiteral("Renamed server");
        layout.views[0].name = QStringLiteral("Renamed view");
        const QByteArray encoded = layout.toJson();
        QVERIFY(!encoded.isEmpty());
        auto restored = WorkspaceLayout::fromJson(encoded);
        QVERIFY(restored.has_value());
        QCOMPARE(restored->accounts[0].id, QStringLiteral("s1"));
        QCOMPARE(restored->accounts[0].name, QStringLiteral("Renamed server"));
        QCOMPARE(restored->views[0].id, QStringLiteral("merged"));
        QCOMPARE(restored->views[0].name, QStringLiteral("Renamed view"));
        QCOMPARE(restored->activeViewId, QStringLiteral("merged"));
        QCOMPARE(restored->toJson(), encoded);
    }

    void persistedAccountIdentityAndLegacyMigration()
    {
        WorkspaceLayout layout = sampleLayout();
        auto& account = layout.accounts[0];
        account.serverBaseUrl = QStringLiteral("https://chat.example.org/mm/");
        account.serverUserId = QStringLiteral("user-42");
        account.loginName = QStringLiteral("alice@example.org");
        QVERIFY(layout.validate());
        const QByteArray encoded = layout.toJson();
        QCOMPARE(QJsonDocument::fromJson(encoded).object().value(QStringLiteral("version")).toInt(), 2);
        QVERIFY(!encoded.contains("password"));
        QVERIFY(!encoded.contains("token"));
        auto restored = WorkspaceLayout::fromJson(encoded);
        QVERIFY(restored.has_value());
        QVERIFY(restored->accounts[0].hasConnectionIdentity());
        QCOMPARE(restored->accounts[0].id, QStringLiteral("s1"));
        QCOMPARE(restored->accounts[0].serverBaseUrl, account.serverBaseUrl);
        QCOMPARE(restored->accounts[0].serverUserId, account.serverUserId);
        QCOMPARE(restored->accounts[0].loginName, account.loginName);

        // Pre-v2 topology contains only an account ID and display name.
        QJsonObject legacy = QJsonDocument::fromJson(encoded).object();
        legacy.insert(QStringLiteral("version"), 1);
        QJsonArray records;
        for (const QJsonValue& item : legacy.value(QStringLiteral("accounts")).toArray()) {
            QJsonObject record = item.toObject();
            record.remove(QStringLiteral("serverBaseUrl"));
            record.remove(QStringLiteral("serverUserId"));
            record.remove(QStringLiteral("loginName"));
            records.append(record);
        }
        legacy.insert(QStringLiteral("accounts"), records);
        auto old = WorkspaceLayout::fromJson(
            QJsonDocument(legacy).toJson(QJsonDocument::Compact));
        QVERIFY(old.has_value());
        QCOMPARE(old->accounts[0].id, QStringLiteral("s1"));
        QVERIFY(!old->accounts[0].hasConnectionIdentity());
    }

    void rejectsPartialDuplicateAndInvalidRemoteAccounts()
    {
        WorkspaceLayout layout = sampleLayout();
        auto& first = layout.accounts[0];
        first.serverBaseUrl = QStringLiteral("https://chat.example.org/mm");
        QVERIFY(!layout.validate());
        first.serverUserId = QStringLiteral("user-1");
        first.loginName = QStringLiteral("alice");
        QVERIFY(layout.validate());
        first.serverBaseUrl = QStringLiteral("file:///tmp/");
        QVERIFY(!layout.validate());
        first.serverBaseUrl = QStringLiteral("https://chat.example.org/mm");
        auto& second = layout.accounts[1];
        second.serverBaseUrl = QStringLiteral("https://CHAT.example.org:443/mm/");
        second.serverUserId = QStringLiteral("user-1");
        second.loginName = QStringLiteral("alice");
        QVERIFY(!layout.validate());
        second.serverUserId = QStringLiteral("user-2");
        QVERIFY(layout.validate());

        QJsonObject malformed = QJsonDocument::fromJson(layout.toJson()).object();
        QJsonArray records = malformed.value(QStringLiteral("accounts")).toArray();
        QJsonObject record = records[0].toObject();
        record.remove(QStringLiteral("serverUserId"));
        records.replace(0, record);
        malformed.insert(QStringLiteral("accounts"), records);
        QVERIFY(!WorkspaceLayout::fromJson(
            QJsonDocument(malformed).toJson(QJsonDocument::Compact)));
    }

    void badMembershipIsRejectedWithoutPartialActivation()
    {
        WorkspaceLayout layout = sampleLayout();
        layout.views[0].accountIds.append(QStringLiteral("s4"));
        QString error;
        QVERIFY(!layout.validate(&error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!layout.transitionTo(QStringLiteral("s4view")).has_value());
        QVERIFY(!layout.selectView(QStringLiteral("s4view")));
        QCOMPARE(layout.activeViewId, QStringLiteral("merged"));
        QVERIFY(layout.toJson().isEmpty());

        layout = sampleLayout();
        layout.views[0].accountIds.append(QStringLiteral("s2"));
        QVERIFY(!layout.validate());
        layout = sampleLayout();
        layout.profiles[0].accountIds.append(QStringLiteral("s1"));
        QVERIFY(!layout.validate());
    }

    void invalidIdsAndUnknownViewsAreRejected()
    {
        WorkspaceLayout layout = sampleLayout();
        QVERIFY(!layout.transitionTo(QStringLiteral("missing")).has_value());
        QVERIFY(!layout.selectView(QStringLiteral("missing")));
        layout.views[1].id = layout.views[0].id;
        QVERIFY(!layout.validate());
        layout = sampleLayout();
        layout.activeViewId = QStringLiteral("missing");
        QVERIFY(!layout.validate());
        layout = sampleLayout();
        layout.profiles[2].accountIds.clear();
        QVERIFY(!layout.validate());
    }

    void malformedOrFutureJsonIsRejected()
    {
        QString error;
        QVERIFY(!WorkspaceLayout::fromJson(QByteArrayLiteral("not json"), &error));
        QVERIFY(!error.isEmpty());
        const QByteArray encoded = sampleLayout().toJson();
        QByteArray future = encoded;
        future.replace("\"version\":2", "\"version\":3");
        QVERIFY(!WorkspaceLayout::fromJson(future, &error));
        QVERIFY(error.contains(QStringLiteral("version")));
        QByteArray badIds = encoded;
        badIds.replace("\"accountIds\":[\"s3\"]",
                       "\"accountIds\":[\"missing\"]");
        QVERIFY(!WorkspaceLayout::fromJson(badIds, &error));
        QVERIFY(!error.isEmpty());
    }
};

QTEST_APPLESS_MAIN(WorkspaceLayoutTest)
#include "WorkspaceLayoutTest.moc"
