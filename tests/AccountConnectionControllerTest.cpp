#include <QtTest>

#include "backend/AccountConnectionController.h"

using namespace Mattermost;

class AccountConnectionControllerTest final : public QObject
{
    Q_OBJECT
private slots:
    void initialRestoreAndVpnRetryUseOneController()
    {
        AccountConnectionController controller;
        QSignalSpy restore(&controller, &AccountConnectionController::savedSessionDue);
        controller.startSavedSession(QStringLiteral("office"));
        QCOMPARE(restore.size(), 1);
        QCOMPARE(restore.takeFirst().at(0).toStringList(),
                 QStringList{QStringLiteral("office")});
        controller.savedSessionFailed(QStringLiteral("office"));
        controller.savedSessionFailed(QStringLiteral("other"));
        QCOMPARE(controller.scheduledCount(), 2);
        controller.resumeOnNetworkChange();
        QCOMPARE(restore.size(), 1);
        const QStringList due = restore.takeFirst().at(0).toStringList();
        QCOMPARE(due.size(), 2);
        QVERIFY(due.contains(QStringLiteral("office")));
        QVERIFY(due.contains(QStringLiteral("other")));
        QCOMPARE(controller.scheduledCount(), 0);
    }

    void canceledAccountNeverReconnects()
    {
        AccountConnectionController controller;
        QSignalSpy restore(&controller, &AccountConnectionController::savedSessionDue);
        controller.savedSessionFailed(QStringLiteral("abandoned"));
        controller.savedSessionFailed(QStringLiteral("admitted"));
        controller.cancel(QStringLiteral("abandoned"));
        controller.resumeOnNetworkChange();
        QCOMPARE(restore.size(), 1);
        QCOMPARE(restore.takeFirst().at(0).toStringList(),
                 QStringList{QStringLiteral("admitted")});
    }

    void socketRetriesShareSchedulingAndNetworkPreemption()
    {
        AccountConnectionController controller;
        QSignalSpy sockets(&controller, &AccountConnectionController::webSocketDue);
        QSignalSpy restores(&controller, &AccountConnectionController::savedSessionDue);
        controller.websocketUnavailable(QStringLiteral("office"));
        // Repeated errors from the same failed socket must not multiply retries.
        controller.websocketUnavailable(QStringLiteral("office"));
        QCOMPARE(controller.scheduledCount(), 1);
        controller.resumeOnNetworkChange();
        QCOMPARE(sockets.size(), 1);
        QCOMPARE(sockets.takeFirst().at(0).toStringList(),
                 QStringList{QStringLiteral("office")});
        QCOMPARE(restores.size(), 0);

        controller.websocketUnavailable(QStringLiteral("office"));
        controller.attemptStarted(QStringLiteral("office"));
        controller.resumeOnNetworkChange();
        QCOMPARE(sockets.size(), 0);
        controller.websocketUnavailable(QStringLiteral("office"));
        controller.resumeOnNetworkChange();
        QCOMPARE(sockets.size(), 1);
    }

    void reconnectContinuityIsExplicitlyDecided()
    {
        AccountConnectionController controller;
        QSignalSpy resolved(&controller, &AccountConnectionController::replayResolved);
        controller.websocketUnavailable(QStringLiteral("office"));
        controller.streamVerified(QStringLiteral("office"), true, true);
        QCOMPARE(controller.scheduledCount(), 0);
        QCOMPARE(resolved.size(), 1);
        QCOMPARE(resolved.takeFirst().at(1).toBool(), true);

        controller.websocketUnavailable(QStringLiteral("office"));
        controller.streamVerified(QStringLiteral("office"), true, false);
        QCOMPARE(resolved.size(), 1);
        QCOMPARE(resolved.takeFirst().at(1).toBool(), false);

        controller.streamVerified(QStringLiteral("office"), false, false);
        QCOMPARE(resolved.size(), 0);
    }

    void lostNetworkChangesAreBoundedByTimer()
    {
        AccountConnectionController controller;
        QSignalSpy due(&controller, &AccountConnectionController::webSocketDue);
        controller.websocketUnavailable(QStringLiteral("office"));
        QTRY_COMPARE_WITH_TIMEOUT(due.size(), 1, 3500);
        QCOMPARE(due.takeFirst().at(0).toStringList(),
                 QStringList{QStringLiteral("office")});
    }
};

QTEST_GUILESS_MAIN(AccountConnectionControllerTest)
#include "AccountConnectionControllerTest.moc"
