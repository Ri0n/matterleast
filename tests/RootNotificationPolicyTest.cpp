#include <QtTest>

#include "notifications/RootNotificationPolicy.h"

using namespace Mattermost;

class RootNotificationPolicyTest : public QObject
{
    Q_OBJECT
private slots:
    void channelWithoutFavoriteOrMentionIsSilent()
    {
        QVERIFY(!shouldNotifyRootPost(false, false, false));
    }
    void favoriteChannelNotifiesWithoutMention()
    {
        QVERIFY(shouldNotifyRootPost(false, false, true));
    }
    void mentionStillNotifies()
    {
        QVERIFY(shouldNotifyRootPost(false, true, false));
        QVERIFY(shouldNotifyRootPost(false, true, true));
    }
    void directAndGroupMessagesUnchanged()
    {
        QVERIFY(shouldNotifyRootPost(true, false, false));
        QVERIFY(shouldNotifyRootPost(true, false, true));
    }
};

QTEST_MAIN(RootNotificationPolicyTest)
#include "RootNotificationPolicyTest.moc"
