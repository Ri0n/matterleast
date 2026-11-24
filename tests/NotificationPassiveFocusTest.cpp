#include <QtTest>

#include <QFile>
#include <QRegularExpression>
#include <QString>

#include "notifications/TaskbarAttentionPolicy.h"

using namespace Mattermost;

Q_DECLARE_METATYPE(TaskbarAttention)

namespace {

QString readSource(const QString& relativePath)
{
    QFile file(QStringLiteral(MATTERLEAST_SOURCE_DIR)
               + QLatin1Char('/') + relativePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString functionSection(const QString& source, const QString& start,
                        const QString& end)
{
    const qsizetype first = source.indexOf(start);
    if (first < 0)
        return {};
    const qsizetype last = source.indexOf(end, first + start.size());
    if (last < 0)
        return {};
    return source.mid(first, last - first);
}

} // namespace

class NotificationPassiveFocusTest : public QObject
{
    Q_OBJECT

private slots:
    void taskbarAttention_data()
    {
        QTest::addColumn<QString>("platform");
        QTest::addColumn<TaskbarAttention>("attention");

        QTest::newRow("x11") << QStringLiteral("xcb") << TaskbarAttention::NativeAlert;
        QTest::newRow("windows") << QStringLiteral("windows") << TaskbarAttention::NativeAlert;
        QTest::newRow("macos") << QStringLiteral("cocoa") << TaskbarAttention::NativeAlert;
        QTest::newRow("wayland") << QStringLiteral("wayland") << TaskbarAttention::LauncherUrgency;
        QTest::newRow("wayland-egl") << QStringLiteral("wayland-egl")
                                    << TaskbarAttention::LauncherUrgency;
        QTest::newRow("other") << QStringLiteral("offscreen") << TaskbarAttention::None;
    }

    void taskbarAttention()
    {
        QFETCH(QString, platform);
        QFETCH(TaskbarAttention, attention);
        QCOMPARE(taskbarAttentionFor(platform, Qt::ApplicationInactive), attention);
        QCOMPARE(taskbarAttentionFor(platform, Qt::ApplicationActive), TaskbarAttention::None);
    }

    void incomingNotificationsMustNotRequestWindowFocus()
    {
        // An offscreen QtTest has no real window manager. Trying to raise an
        // actual window cannot prove stacking behavior and crashes on some
        // Linux Qt platform plugins. Guard the concrete production delivery
        // path against direct window activation or an unguarded alert().
        const QString source = readSource(
            QStringLiteral("sources/notifications/AccountPostNotifications.cpp"));
        QVERIFY2(!source.isEmpty(), "Cannot read account notification source");
        const QString delivery = functionSection(
            source, QStringLiteral("void AccountPostNotifications::notify("),
            QStringLiteral("} // namespace Mattermost"));
        QVERIFY2(!delivery.isEmpty(), "Cannot locate notification implementation");
        QVERIFY(delivery.contains(QStringLiteral("notifications_.show(")));
        QVERIFY(delivery.contains(
            QStringLiteral("notifications_.requestTaskbarAttention(*window)")));

        // Match calls at the beginning of a statement. Only the platform
        // policy tested above may route a notification to QApplication::alert.
        const QRegularExpression unwanted(QStringLiteral(
            R"((?:^|\n)\s*(?:QApplication::alert|(?:this->)?(?:raise|activateWindow|showNormal))\s*\()"));
        QVERIFY2(!delivery.contains(unwanted),
                 "Passive message delivery may not request window focus");
    }

    void userClickStillActivatesAndNavigates()
    {
        // A notification action is intentional navigation and is exempt from
        // the passive delivery restriction.
        const QString source = readSource(QStringLiteral("sources/MainWindowRealtime.cpp"));
        QVERIFY2(!source.isEmpty(), "Cannot read MainWindowRealtime source");
        const QString activation = functionSection(
            source,
            QStringLiteral("connect(notificationManager, &NotificationManager::activated,"),
            QStringLiteral("// Server-originated ephemeral posts"));
        QVERIFY2(!activation.isEmpty(), "Cannot locate notification action handler");
        QVERIFY(activation.contains(QStringLiteral("raise();")));
        QVERIFY(activation.contains(QStringLiteral("activateWindow();")));
        QVERIFY(activation.contains(QStringLiteral("openPost(target.postId)")));

        const QString manager = readSource(
            QStringLiteral("sources/notifications/NotificationManager.cpp"));
        QVERIFY2(!manager.isEmpty(), "Cannot read NotificationManager source");
        const QString passive = functionSection(
            manager, QStringLiteral("void NotificationManager::show("),
            QStringLiteral("void NotificationManager::requestTaskbarAttention("));
        QVERIFY2(!passive.isEmpty(), "Cannot locate notification delivery implementation");
        QVERIFY(!passive.contains(QStringLiteral("emit activated(")));
    }
};

QTEST_GUILESS_MAIN(NotificationPassiveFocusTest)
#include "NotificationPassiveFocusTest.moc"
