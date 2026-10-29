#include <QtTest>

#include "backend/PresenceActivityPolicy.h"
#include "backend/PresenceWebSocketPayload.h"

using Mattermost::PresenceActivityPolicy;

class PresenceActivityPolicyTest final : public QObject
{
    Q_OBJECT

private slots:
    void automaticWebSocketPresenceNeverOverridesManualStatus()
    {
        const QJsonObject online = Mattermost::automaticPresenceData(true);
        const QJsonObject idle = Mattermost::automaticPresenceData(false);
        QCOMPARE(online.value(QStringLiteral("user_is_active")).toBool(), true);
        QCOMPARE(online.value(QStringLiteral("manual")).toBool(), false);
        QCOMPARE(idle.value(QStringLiteral("user_is_active")).toBool(), false);
        QCOMPARE(idle.value(QStringLiteral("manual")).toBool(), false);
        QCOMPARE(online.size(), 2);
        QCOMPARE(idle.size(), 2);
    }

    void sendsOncePerMinuteWhileActive()
    {
        PresenceActivityPolicy policy;
        QVERIFY(policy.shouldSendActive(1000, 200, true));
        QVERIFY(!policy.shouldSendActive(2000, 0, true));
        QVERIFY(!policy.shouldSendActive(60999, 0, true));
        QVERIFY(policy.shouldSendActive(61000, 0, true));
        QVERIFY(!policy.shouldSendActive(61001, 0, true));
    }

    void idleThresholdAndResume()
    {
        PresenceActivityPolicy policy;
        QVERIFY(policy.shouldSendActive(1000, 0, true));
        QVERIFY(!policy.shouldSendActive(2000, 60000, true));
        // Just over 60s idle; the server decides when status becomes Away.
        QVERIFY(!policy.shouldSendActive(3000, 60001, true));
        QVERIFY(policy.shouldSendActive(4000, 100, true));
        QVERIFY(!policy.shouldSendActive(4001, 0, true));
    }

    void disconnectionAndReconnect()
    {
        PresenceActivityPolicy policy;
        QVERIFY(policy.shouldSendActive(1000, 0, true));
        QVERIFY(!policy.shouldSendActive(2000, 0, false));
        QVERIFY(policy.shouldSendActive(3000, 0, true));
    }

    void unknownIdleDoesNotClaimUserIsOnline()
    {
        PresenceActivityPolicy policy;
        QVERIFY(!policy.shouldSendActive(1000, -1, true));
        QVERIFY(policy.shouldSendActive(2000, 0, true));
        QVERIFY(!policy.shouldSendActive(3000, -1, true));
        QVERIFY(policy.shouldSendActive(4000, 0, true));
    }

    void monotonicClockDiscontinuityRecovers()
    {
        PresenceActivityPolicy policy;
        QVERIFY(policy.shouldSendActive(10000, 0, true));
        QVERIFY(policy.shouldSendActive(100, 0, true));
    }
};

QTEST_MAIN(PresenceActivityPolicyTest)
#include "PresenceActivityPolicyTest.moc"
