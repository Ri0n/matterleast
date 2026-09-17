#include <QtTest>

#include "navigation/NavigationRequestGate.h"

using namespace Mattermost;

class NavigationRequestGateTest : public QObject
{
    Q_OBJECT

private slots:
    void newerRequestSupersedesOlderCallback()
    {
        NavigationRequestGate gate;
        const quint64 first = gate.begin();
        QVERIFY(gate.isCurrent(first));

        const quint64 second = gate.begin();
        QVERIFY(second > first);
        QVERIFY(!gate.isCurrent(first));
        QVERIFY(gate.isCurrent(second));
    }
};

QTEST_APPLESS_MAIN(NavigationRequestGateTest)
#include "NavigationRequestGateTest.moc"
