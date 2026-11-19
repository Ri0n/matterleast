#include <QtTest>

#include <QFile>
#include <QRegularExpression>
#include <QString>

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
    void incomingNotificationsMustNotRequestWindowFocus()
    {
        // An offscreen QtTest has no real window manager. Trying to raise an
        // actual window cannot prove stacking behavior and crashes on some
        // Linux Qt platform plugins. Guard the concrete production delivery
        // path instead: new messages may publish notifications, but may not
        // make any window-manager attention or activation requests.
        const QString source = readSource(QStringLiteral("sources/mainwindow.cpp"));
        QVERIFY2(!source.isEmpty(), "Cannot read MainWindow source");
        const QString delivery = functionSection(
            source, QStringLiteral("void MainWindow::messageNotify("),
            QStringLiteral("void MainWindow::unreadMessagesNotify("));
        QVERIFY2(!delivery.isEmpty(), "Cannot locate messageNotify implementation");
        QVERIFY(delivery.contains(QStringLiteral("notificationManager->show(")));

        // Match actual calls at the beginning of a statement, not comments
        // describing APIs. QApplication::alert is included because a desktop
        // may raise the window while handling its 'attention' request.
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
            QStringLiteral("connect(notificationManager.get(), &NotificationManager::activated,"),
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
            QStringLiteral("void NotificationManager::onFallbackMessageClicked("));
        QVERIFY2(!passive.isEmpty(), "Cannot locate notification delivery implementation");
        QVERIFY(!passive.contains(QStringLiteral("emit activated(")));
    }
};

QTEST_GUILESS_MAIN(NotificationPassiveFocusTest)
#include "NotificationPassiveFocusTest.moc"
