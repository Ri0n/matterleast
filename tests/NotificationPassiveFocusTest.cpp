#include <QtTest>

#include <QApplication>
#include <QIcon>
#include <QPixmap>
#include <QSystemTrayIcon>
#include <QWidget>

#include "notifications/NotificationManager.h"

using namespace Mattermost;

class NotificationPassiveFocusTest : public QObject
{
    Q_OBJECT

private slots:
    void incomingThreadReplyDoesNotActivateAnotherWindow()
    {
        QPixmap icon(16, 16);
        icon.fill(Qt::white);
        QSystemTrayIcon tray(QIcon(icon));
        NotificationManager notifications(tray);

        QWidget mainWindow;
        mainWindow.setWindowTitle(QStringLiteral("MatterLeast"));
        mainWindow.show();

        QWidget otherWindow;
        otherWindow.setWindowTitle(QStringLiteral("External browser"));
        otherWindow.show();
        otherWindow.raise();
        otherWindow.activateWindow();
        QApplication::processEvents();

        // Offscreen platforms are not obliged to provide native focus. When
        // one does, we can additionally check that it remains unchanged.
        QWidget* previousActiveWindow = QApplication::activeWindow();
        int explicitActivations = 0;
        QObject::connect(&notifications, &NotificationManager::activated,
                         &notifications, [&](const NotificationTarget&) {
            ++explicitActivations;
        });

        const NotificationTarget threadReply {
            QStringLiteral("channel-id"),
            QStringLiteral("reply-id"),
            QStringLiteral("root-id"),
        };
        notifications.show(QStringLiteral("Replied in a followed thread"),
                           QStringLiteral("Another message"), threadReply);
        QApplication::processEvents();

        // Delivery and a notification action are separate events. A passive
        // new message must neither navigate nor steal focus from the browser.
        QCOMPARE(explicitActivations, 0);
        if (previousActiveWindow)
            QCOMPARE(QApplication::activeWindow(), previousActiveWindow);

        // Consecutive replies remain passive too, including when no input
        // was sent to the foreground window between them.
        notifications.show(QStringLiteral("Replied again"),
                           QStringLiteral("One more message"), threadReply);
        QApplication::processEvents();
        QCOMPARE(explicitActivations, 0);
        if (previousActiveWindow)
            QCOMPARE(QApplication::activeWindow(), previousActiveWindow);
    }
};

QTEST_MAIN(NotificationPassiveFocusTest)
#include "NotificationPassiveFocusTest.moc"
