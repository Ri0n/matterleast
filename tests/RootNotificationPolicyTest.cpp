#include <QtTest>

#include "Settings.h"
#include "notifications/RootNotificationPolicy.h"

using namespace Mattermost;

class RootNotificationPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void defaultsToMentionsOrFavorites()
    {
        QCOMPARE(CHANNEL_ROOT_NOTIFICATION_MODE_DEFAULT,
                 static_cast<int>(ChannelRootNotificationMode::MentionsOrFavorites));
        QCOMPARE(channelRootNotificationModeFromSetting(
                     CHANNEL_ROOT_NOTIFICATION_MODE_DEFAULT),
                 ChannelRootNotificationMode::MentionsOrFavorites);
        // Invalid persisted values must never unexpectedly enable notifications
        // for every channel.
        QCOMPARE(channelRootNotificationModeFromSetting(-1),
                 ChannelRootNotificationMode::MentionsOrFavorites);
        QCOMPARE(channelRootNotificationModeFromSetting(999),
                 ChannelRootNotificationMode::MentionsOrFavorites);
    }

    void policyMatrix_data()
    {
        QTest::addColumn<int>("setting");
        QTest::addColumn<bool>("mentioned");
        QTest::addColumn<bool>("favorite");
        QTest::addColumn<bool>("expected");

        // For ordinary public/private channels, mentions always notify.
        // Mode 0 ignores favorites, mode 1 includes them, mode 2 includes all.
        for (int mode = 0; mode <= 2; ++mode) {
            for (int mention = 0; mention <= 1; ++mention) {
                for (int favorite = 0; favorite <= 1; ++favorite) {
                    const bool expected = mention != 0
                        || mode == 2
                        || (mode == 1 && favorite != 0);
                    const QByteArray row = QStringLiteral("mode%1_mention%2_favorite%3")
                        .arg(mode).arg(mention).arg(favorite).toLatin1();
                    QTest::newRow(row.constData())
                        << mode << bool(mention) << bool(favorite) << expected;
                }
            }
        }
    }

    void policyMatrix()
    {
        QFETCH(int, setting);
        QFETCH(bool, mentioned);
        QFETCH(bool, favorite);
        QFETCH(bool, expected);
        QVERIFY(channelRootNotificationModeFromSetting(setting)
                != ChannelRootNotificationMode::MentionsOrFavorites
                || setting == CHANNEL_ROOT_NOTIFICATION_MODE_DEFAULT);
        QCOMPARE(shouldNotifyRootPost(
                     false, mentioned, favorite,
                     channelRootNotificationModeFromSetting(setting)),
                 expected);
    }

    void directAndGroupMessagesUnchanged()
    {
        for (int mode = 0; mode <= 2; ++mode) {
            const auto selected = channelRootNotificationModeFromSetting(mode);
            QVERIFY(shouldNotifyRootPost(true, false, false, selected));
            QVERIFY(shouldNotifyRootPost(true, false, true, selected));
        }
    }
};

QTEST_MAIN(RootNotificationPolicyTest)
#include "RootNotificationPolicyTest.moc"
