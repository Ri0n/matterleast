#include <QtTest>
#include "update/WindowsUpdateVersionPolicy.h"

using Mattermost::windowsUpdateAlreadyCurrentOrOlder;

class WindowsUpdateVersionPolicyTest : public QObject
{
    Q_OBJECT
private slots:
    void doesNotDownloadOwnNightlyBuild()
    {
        const QString id = QStringLiteral("nightly-20261028-185307-a743adbc1dfb");
        QVERIFY(windowsUpdateAlreadyCurrentOrOlder(
            QStringLiteral("nightly"), id, id,
            QVersionNumber(2, 0), QVersionNumber(2, 0)));
    }

    void downloadsNewerNightly()
    {
        QVERIFY(!windowsUpdateAlreadyCurrentOrOlder(
            QStringLiteral("nightly"),
            QStringLiteral("nightly-20261028-185307-a743adbc1dfb"),
            QStringLiteral("nightly-20261029-010000-aaaaaaaaaaaa"),
            QVersionNumber(2, 0), QVersionNumber(2, 0)));
    }

    void doesNotDowngradeNightly()
    {
        QVERIFY(windowsUpdateAlreadyCurrentOrOlder(
            QStringLiteral("nightly"),
            QStringLiteral("nightly-20261029-010000-aaaaaaaaaaaa"),
            QStringLiteral("nightly-20261028-185307-a743adbc1dfb"),
            QVersionNumber(2, 0), QVersionNumber(2, 0)));
    }

    void allowsFirstNightlyFromStable()
    {
        QVERIFY(!windowsUpdateAlreadyCurrentOrOlder(
            QStringLiteral("nightly"), QStringLiteral("v2.0"),
            QStringLiteral("nightly-20261028-185307-a743adbc1dfb"),
            QVersionNumber(2, 0), QVersionNumber(2, 0)));
    }

    void respectsNumericVersionForStableAndDowngrades()
    {
        QVERIFY(windowsUpdateAlreadyCurrentOrOlder(
            QStringLiteral("stable"), QStringLiteral("v2.0"), QStringLiteral("v2.0"),
            QVersionNumber(2, 0), QVersionNumber(2, 0)));
        QVERIFY(!windowsUpdateAlreadyCurrentOrOlder(
            QStringLiteral("stable"), QStringLiteral("nightly-20261028-185307-a743adbc1dfb"),
            QStringLiteral("v2.1"), QVersionNumber(2, 0), QVersionNumber(2, 1)));
        QVERIFY(windowsUpdateAlreadyCurrentOrOlder(
            QStringLiteral("nightly"), QStringLiteral("v3.0"),
            QStringLiteral("nightly-20261028-185307-a743adbc1dfb"),
            QVersionNumber(3, 0), QVersionNumber(2, 0)));
    }
};

QTEST_APPLESS_MAIN(WindowsUpdateVersionPolicyTest)
#include "WindowsUpdateVersionPolicyTest.moc"
