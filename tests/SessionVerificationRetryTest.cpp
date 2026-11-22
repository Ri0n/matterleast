#include <QtTest>

#include "backend/SessionVerificationRetry.h"

using namespace Mattermost;

class SessionVerificationRetryTest final : public QObject
{
    Q_OBJECT
private slots:
    void transientNetworkFailureThenSuccess()
    {
        QObject owner;
        auto* retry = new SessionVerificationRetry(&owner);
        int checks = 0;
        int completed = 0;
        bool authenticated = false;
        retry->start([&](SessionVerificationRetry::CheckDone done) {
            ++checks;
            done(checks == 2, checks == 1);
        }, [&](bool success, bool temporary) {
            ++completed;
            authenticated = success;
            QVERIFY(!temporary);
        });
        QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 6500);
        QCOMPARE(checks, 2);
        QVERIFY(authenticated);
    }

    void rejectedCredentialMustNotRetry()
    {
        QObject owner;
        auto* retry = new SessionVerificationRetry(&owner);
        int checks = 0;
        int completed = 0;
        retry->start([&](SessionVerificationRetry::CheckDone done) {
            ++checks;
            done(false, false);
        }, [&](bool success, bool temporary) {
            ++completed;
            QVERIFY(!success);
            QVERIFY(!temporary);
        });
        QCOMPARE(checks, 1);
        QCOMPARE(completed, 1);
    }

    void temporaryErrorsStopAfterBoundedAttempts()
    {
        QObject owner;
        auto* retry = new SessionVerificationRetry(&owner);
        int checks = 0;
        int completed = 0;
        retry->start([&](SessionVerificationRetry::CheckDone done) {
            ++checks;
            done(false, true);
        }, [&](bool success, bool temporary) {
            ++completed;
            QVERIFY(!success);
            QVERIFY(temporary);
        });
        QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 9500);
        QCOMPARE(checks, SessionVerificationRetry::MaxAttempts);
    }

    void cancellationStopsQueuedRetriesAndLateCallbacks()
    {
        QObject owner;
        auto* retry = new SessionVerificationRetry(&owner);
        int checks = 0;
        int completed = 0;
        SessionVerificationRetry::CheckDone lateCompletion;
        retry->start([&](SessionVerificationRetry::CheckDone done) {
            ++checks;
            if (checks == 1) {
                done(false, true);
            } else {
                lateCompletion = std::move(done);
            }
        }, [&](bool, bool) { ++completed; });
        QCOMPARE(checks, 1);
        retry->cancel();
        QTest::qWait(1650);
        QCOMPARE(checks, 1);
        QCOMPARE(completed, 0);
        if (lateCompletion) lateCompletion(true, false);
        QCOMPARE(completed, 0);
    }
};

QTEST_APPLESS_MAIN(SessionVerificationRetryTest)
#include "SessionVerificationRetryTest.moc"
