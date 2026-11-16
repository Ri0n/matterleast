#include <QtTest>

#include "chat-area/outgoing-post/VoiceRecordingPolicy.h"

using namespace Mattermost;

class VoiceRecordingPolicyTest : public QObject
{
    Q_OBJECT
private slots:
    void microphoneOnlyForEmptyComposer()
    {
        using VoiceRecordingPolicy::canOfferMicrophone;
        QVERIFY(canOfferMicrophone(false, false, false, false));
        QVERIFY(!canOfferMicrophone(true, false, false, false));
        QVERIFY(!canOfferMicrophone(false, true, false, false));
        QVERIFY(!canOfferMicrophone(false, false, true, false));
        QVERIFY(!canOfferMicrophone(false, false, false, true));
    }

    void fileSizeBound()
    {
        using VoiceRecordingPolicy::withinLimit;
        QVERIFY(!withinLimit(0));
        QVERIFY(!withinLimit(-1));
        QVERIFY(withinLimit(1));
        // Decimal megabytes, not binary mebibytes.
        QCOMPARE(VoiceRecordingPolicy::MaxVoiceBytes, qint64(100000000));
        QVERIFY(withinLimit(99999999));
        QVERIFY(!withinLimit(100000000));
        QVERIFY(!withinLimit(100000001));
        QVERIFY(!withinLimit(104857600)); // Old, incorrect 100 MiB boundary.
        QCOMPARE(VoiceRecordingPolicy::ContainerFinalizationReserve, qint64(5000000));
        QVERIFY(!VoiceRecordingPolicy::shouldStopForSize(94999999));
        QVERIFY(VoiceRecordingPolicy::shouldStopForSize(95000000));
        QVERIFY(VoiceRecordingPolicy::shouldStopForSize(100000000));
    }

    void recordingClockHasNoArbitraryDurationLimit()
    {
        using VoiceRecordingPolicy::elapsedLabel;
        QCOMPARE(elapsedLabel(0), QStringLiteral("00:00"));
        QCOMPARE(elapsedLabel(999), QStringLiteral("00:00"));
        QCOMPARE(elapsedLabel(61000), QStringLiteral("01:01"));
        QCOMPARE(elapsedLabel(3600000), QStringLiteral("1:00:00"));
        QCOMPARE(elapsedLabel(25LL * 3600000 + 61000),
                 QStringLiteral("25:01:01"));
    }
};

QTEST_MAIN(VoiceRecordingPolicyTest)
#include "VoiceRecordingPolicyTest.moc"
