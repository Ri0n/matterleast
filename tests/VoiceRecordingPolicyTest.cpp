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
        QVERIFY(withinLimit(VoiceRecordingPolicy::MaxVoiceBytes));
        QVERIFY(!withinLimit(VoiceRecordingPolicy::MaxVoiceBytes + 1));
        QCOMPARE(VoiceRecordingPolicy::MaxVoiceBytes, qint64(104857600));
        QVERIFY(!VoiceRecordingPolicy::shouldStopForSize(
            VoiceRecordingPolicy::MaxVoiceBytes - VoiceRecordingPolicy::ContainerFinalizationReserve - 1));
        QVERIFY(VoiceRecordingPolicy::shouldStopForSize(
            VoiceRecordingPolicy::MaxVoiceBytes - VoiceRecordingPolicy::ContainerFinalizationReserve));
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
