#include <QtTest>
#include <limits>

#include "ui/AccountTrayBadgeModel.h"
#include "ui/TrayBadgePolicy.h"

using namespace Mattermost;

class AccountTrayBadgeModelTest final : public QObject
{
    Q_OBJECT
private slots:
    void preservesBackgroundAttentionAndAggregatesAcrossAccounts()
    {
        QObject first, second;
        AccountTrayBadgeModel badges;
        badges.update(&first, 3, false);
        badges.update(&second, 4, true);
        QCOMPARE(badges.aggregate().attentionCount, std::uint32_t(7));
        QVERIFY(badges.aggregate().hasUnreadChannel);
        QCOMPARE(trayBadgeKind(badges.aggregate().attentionCount,
                               badges.aggregate().hasUnreadChannel),
                 TrayBadgeKind::Attention);

        // The most recent update is not an account selector.
        badges.update(&second, 0, false);
        QCOMPARE(badges.aggregate().attentionCount, std::uint32_t(3));
        QVERIFY(!badges.aggregate().hasUnreadChannel);
        badges.update(&first, 0, true);
        QCOMPARE(trayBadgeKind(badges.aggregate().attentionCount,
                               badges.aggregate().hasUnreadChannel),
                 TrayBadgeKind::ChannelUnread);
        badges.remove(&first);
        QCOMPARE(badges.aggregate().attentionCount, std::uint32_t(0));
        QCOMPARE(trayBadgeKind(badges.aggregate().attentionCount,
                               badges.aggregate().hasUnreadChannel),
                 TrayBadgeKind::None);
    }

    void saturatesInsteadOfOverflowingAndDropsOnlyRemovedAccount()
    {
        QObject first, second;
        AccountTrayBadgeModel badges;
        badges.update(&first, std::numeric_limits<std::uint32_t>::max(), false);
        badges.update(&second, 123, true);
        QCOMPARE(badges.aggregate().attentionCount,
                 std::numeric_limits<std::uint32_t>::max());
        QVERIFY(badges.aggregate().hasUnreadChannel);
        badges.remove(&first);
        QCOMPARE(badges.aggregate().attentionCount, std::uint32_t(123));
        QVERIFY(badges.aggregate().hasUnreadChannel);
        badges.remove(&second);
        QCOMPARE(badges.aggregate().attentionCount, std::uint32_t(0));
    }
};

QTEST_MAIN(AccountTrayBadgeModelTest)
#include "AccountTrayBadgeModelTest.moc"
