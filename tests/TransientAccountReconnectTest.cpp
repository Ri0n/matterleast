#include <QtTest>

#include "backend/TransientAccountReconnect.h"

using namespace Mattermost;

class TransientAccountReconnectTest final : public QObject
{
    Q_OBJECT
private slots:
    void vpnChangePreemptsAllOfflineAccountDelays()
    {
        TransientAccountReconnect reconnect;
        QSignalSpy due(&reconnect, &TransientAccountReconnect::retryDue);
        reconnect.schedule(QStringLiteral("office"));
        reconnect.schedule(QStringLiteral("other"));
        QCOMPARE(reconnect.scheduledCount(), 2);
        QCOMPARE(due.size(), 0);

        reconnect.resumeOnNetworkChange();
        QCOMPARE(due.size(), 1);
        QCOMPARE(reconnect.scheduledCount(), 0);
        const QStringList ids = due.takeFirst().at(0).toStringList();
        QCOMPARE(ids.size(), 2);
        QVERIFY(ids.contains(QStringLiteral("office")));
        QVERIFY(ids.contains(QStringLiteral("other")));
        reconnect.resumeOnNetworkChange();
        QCOMPARE(due.size(), 0);
    }

    void canceledLoginAndInactiveProfileCannotReconnect()
    {
        TransientAccountReconnect reconnect;
        QSignalSpy due(&reconnect, &TransientAccountReconnect::retryDue);
        reconnect.schedule(QStringLiteral("abandoned"));
        reconnect.schedule(QStringLiteral("admitted"));
        reconnect.cancel(QStringLiteral("abandoned"));
        QVERIFY(!reconnect.isScheduled(QStringLiteral("abandoned")));
        reconnect.resumeOnNetworkChange();
        QCOMPARE(due.size(), 1);
        QCOMPARE(due.takeFirst().at(0).toStringList(),
                 QStringList{QStringLiteral("admitted")});
    }

    void ongoingRetryIsNotRelaunchedByNetworkChange()
    {
        TransientAccountReconnect reconnect;
        QSignalSpy due(&reconnect, &TransientAccountReconnect::retryDue);
        reconnect.schedule(QStringLiteral("office"));
        reconnect.attemptStarted(QStringLiteral("office"));
        reconnect.resumeOnNetworkChange();
        QCOMPARE(due.size(), 0);

        // A new temporary failure reschedules the same account, retaining
        // its increasing backoff until a verified success or explicit cancel.
        reconnect.schedule(QStringLiteral("office"));
        QCOMPARE(reconnect.scheduledCount(), 1);
        reconnect.resumeOnNetworkChange();
        QCOMPARE(due.size(), 1);
    }
};

QTEST_GUILESS_MAIN(TransientAccountReconnectTest)
#include "TransientAccountReconnectTest.moc"
