#include <QtTest>

#include "backend/ChannelActivityTracker.h"
#include "backend/DeletedUnreadPostPolicy.h"

using namespace Mattermost;

namespace {

void setMembership(ChannelActivityTracker& tracker,
                   uint64_t lastViewedAt = 1000,
                   uint64_t readMessages = 5,
                   uint64_t readRootMessages = 5,
                   bool hasRootMessages = true,
                   uint64_t mentions = 0,
                   uint64_t rootMentions = 0,
                   bool hasRootMentions = true,
                   bool muted = false)
{
    tracker.setMembership(QStringLiteral("channel"), lastViewedAt,
                          readMessages, readRootMessages, hasRootMessages,
                          mentions, rootMentions, hasRootMentions, muted);
}

void synchronize(ChannelActivityTracker& tracker,
                 uint64_t lastPostAt = 2000,
                 uint64_t totalMessages = 5,
                 uint64_t totalRootMessages = 5,
                 bool hasRootMessages = true,
                 bool collapsedThreads = false)
{
    tracker.synchronizeChannel(QStringLiteral("channel"), lastPostAt,
                               totalMessages, totalRootMessages,
                               hasRootMessages, collapsedThreads);
}

} // namespace

class ChannelActivityTrackerTest : public QObject
{
    Q_OBJECT

private slots:
    void deletedEventChecksOnlyRelevantUnreadMessages()
    {
        const QString me = QStringLiteral("me");
        const QString other = QStringLiteral("other");
        QVERIFY(deletionMayAffectChannelUnread(2000, 1000, other, me, false, false));
        QVERIFY(!deletionMayAffectChannelUnread(999, 1000, other, me, false, false));
        QVERIFY(!deletionMayAffectChannelUnread(2000, 1000, me, me, false, false));
        QVERIFY(!deletionMayAffectChannelUnread(2000, 1000, other, me, true, true));
        QVERIFY(deletionMayAffectChannelUnread(2000, 1000, other, me, true, false));
        QVERIFY(deletionMayAffectChannelUnread(0, 1000, QString(), me, false, true));
    }

    void deletedUnreadReconciliationNeedsCompleteServerProof()
    {
        const QString me = QStringLiteral("me");
        const uint64_t boundary = 1000;
        const auto check = [&](const QJsonObject& body) {
            return checkLiveUnreadPostsAfterDeletion(body, boundary, me);
        };

        const QJsonObject emptyResponse {
            {QStringLiteral("order"), QJsonArray {}},
            {QStringLiteral("posts"), QJsonObject {}},
        };
        QCOMPARE(check(emptyResponse), DeletedUnreadPostCheck::NoUnreadPosts);
        QCOMPARE(check(QJsonObject {}), DeletedUnreadPostCheck::Inconclusive);
        QCOMPARE(check(QJsonObject {
            {QStringLiteral("order"), QJsonArray {QStringLiteral("missing")}},
            {QStringLiteral("posts"), QJsonObject {}},
        }), DeletedUnreadPostCheck::Inconclusive);

        const QJsonObject foreignPost {
            {QStringLiteral("id"), QStringLiteral("post1")},
            {QStringLiteral("user_id"), QStringLiteral("other")},
            {QStringLiteral("create_at"), 2000},
            {QStringLiteral("delete_at"), 0},
            {QStringLiteral("root_id"), QString()},
        };
        const auto response = [](const QJsonObject& post) {
            return QJsonObject {
                {QStringLiteral("order"), QJsonArray {QStringLiteral("post1")}},
                {QStringLiteral("posts"), QJsonObject {
                    {QStringLiteral("post1"), post},
                }},
            };
        };
        QCOMPARE(check(response(foreignPost)),
                 DeletedUnreadPostCheck::HasUnreadPosts);

        QJsonObject deleted = foreignPost;
        deleted.insert(QStringLiteral("delete_at"), 2100);
        QCOMPARE(check(response(deleted)), DeletedUnreadPostCheck::NoUnreadPosts);
        deleted.remove(QStringLiteral("create_at"));
        QCOMPARE(check(response(deleted)), DeletedUnreadPostCheck::Inconclusive);

        QJsonObject own = foreignPost;
        own.insert(QStringLiteral("user_id"), me);
        QCOMPARE(check(response(own)), DeletedUnreadPostCheck::NoUnreadPosts);

        QJsonObject oldEdit = foreignPost;
        oldEdit.insert(QStringLiteral("create_at"), 500);
        oldEdit.insert(QStringLiteral("update_at"), 3000);
        QCOMPARE(check(response(oldEdit)), DeletedUnreadPostCheck::NoUnreadPosts);

        QJsonObject routine = foreignPost;
        routine.insert(QStringLiteral("type"), QStringLiteral("system_join_leave"));
        QCOMPARE(check(response(routine)), DeletedUnreadPostCheck::NoUnreadPosts);

        // An unread reply is a real unread in the all-post domain. A deletion
        // must not mark the entire channel read and wipe thread progress.
        QJsonObject reply = foreignPost;
        reply.insert(QStringLiteral("root_id"), QStringLiteral("thread-root"));
        QCOMPARE(check(response(reply)), DeletedUnreadPostCheck::HasUnreadPosts);

        QJsonObject validDeleted = foreignPost;
        validDeleted.insert(QStringLiteral("delete_at"), 2100);
        QJsonObject multiple = emptyResponse;
        multiple.insert(QStringLiteral("posts"), QJsonObject {
            {QStringLiteral("post1"), validDeleted},
            {QStringLiteral("post2"), QJsonObject {
                {QStringLiteral("id"), QStringLiteral("post2")},
                {QStringLiteral("user_id"), QStringLiteral("other")},
                {QStringLiteral("create_at"), 2200},
                {QStringLiteral("delete_at"), 0},
            }},
        });
        QCOMPARE(check(multiple), DeletedUnreadPostCheck::HasUnreadPosts);
    }

    void unreadBadgeCountsRespectCrtAndOwnPostCredits()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 3);
        synchronize(tracker, 2000, 11, 6, true, false);
        QCOMPARE(tracker.unreadCount(QStringLiteral("channel"), 11, 6),
                 uint64_t(6));
        synchronize(tracker, 2000, 11, 6, true, true);
        QCOMPARE(tracker.unreadCount(QStringLiteral("channel"), 11, 6),
                 uint64_t(3));

        // Channel root-only timelines show only root posts even when CRT is
        // disabled and replies count for channel notifications.
        synchronize(tracker, 2000, 11, 6, true, false);
        QCOMPARE(tracker.unreadCount(QStringLiteral("channel"), 11, 6, true),
                 uint64_t(3));

        // The count is a server membership fact, independent of mute policy.
        tracker.setMuted(QStringLiteral("channel"), true);
        // Disabling CRT switches the default to all-post counters; mute
        // changes attention policy, not membership counts.
        QCOMPARE(tracker.unreadCount(QStringLiteral("channel"), 11, 6),
                 uint64_t(6));
        QCOMPARE(tracker.unreadCount(QStringLiteral("channel"), 11, 6, true),
                 uint64_t(3));

        // An own post not yet reflected in the membership gets one count
        // credit until the server catches up.
        tracker.recordPost(QStringLiteral("channel"), 2100, true, false, false);
        synchronize(tracker, 2100, 12, 7, true, true);
        QCOMPARE(tracker.unreadCount(QStringLiteral("channel"), 12, 7),
                 uint64_t(3));

        tracker.markUnread(QStringLiteral("channel"), 1499,
                           6, 4, true, 0, 0, true);
        QCOMPARE(tracker.unreadCount(QStringLiteral("channel"), 12, 7),
                 uint64_t(3));
    }

    void ordinaryCountsSeedUnreadWhenCrtIsOff()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 2000, 7, 5, true, false);

        QVERIFY(tracker.isTracked(QStringLiteral("channel")));
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void rootCountsSeedUnreadWhenCrtIsOn()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 20, 5);
        synchronize(tracker, 2000, 20, 7, true, true);

        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void rootFieldsDoNotChangeUnreadSemanticsWhenCrtIsOff()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 10, 5);

        // The ordinary channel has two unread messages while its root counters
        // happen to be equal. Merely having CRT fields in JSON must not switch
        // the comparison domain when Collapsed Reply Threads is disabled.
        synchronize(tracker, 2000, 12, 5, true, false);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));

        tracker.recordViewed(QStringLiteral("channel"), 2000, 12, 5, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        // With CRT enabled the same root counters correctly say there is no
        // unread root activity.
        synchronize(tracker, 2000, 12, 5, true, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
    }

    void crtRootUnreadModeRequiresBothRootCounters()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 20, 5, true);
        synchronize(tracker, 2000, 20, 5, true, true);
        QVERIFY(tracker.usesRootUnreadCounts(QStringLiteral("channel")));

        ChannelActivityTracker noChannelRootCount;
        setMembership(noChannelRootCount, 1000, 20, 5, true);
        synchronize(noChannelRootCount, 2000, 20, 0, false, true);
        QVERIFY(!noChannelRootCount.usesRootUnreadCounts(QStringLiteral("channel")));

        ChannelActivityTracker noMembershipRootCount;
        setMembership(noMembershipRootCount, 1000, 20, 0, false);
        synchronize(noMembershipRootCount, 2000, 20, 5, true, true);
        QVERIFY(!noMembershipRootCount.usesRootUnreadCounts(QStringLiteral("channel")));
    }

    void crtFallbackReclassifiesReplyRuntimeActivity()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5, true, 0, 0, true, false);
        synchronize(tracker, 1000, 5, 5, true, true);
        QVERIFY(tracker.usesRootUnreadCounts(QStringLiteral("channel")));

        tracker.recordPost(QStringLiteral("channel"), 2000, false, true, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));

        // Reading the root-only parent must not consume the hidden reply's
        // all-message/runtime state.
        tracker.recordViewed(QStringLiteral("channel"), 1000, 6, 5, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));

        // If the channel-side root counter disappears, that same already
        // received reply belongs to the whole-message fallback domain.
        synchronize(tracker, 2000, 6, 0, false, true);
        QVERIFY(!tracker.usesRootUnreadCounts(QStringLiteral("channel")));
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(tracker.hasMention(QStringLiteral("channel")));

        // Restoring authoritative root counters moves the reply back to
        // thread-only activity without losing its runtime bookkeeping.
        synchronize(tracker, 2000, 6, 5, true, true);
        QVERIFY(tracker.usesRootUnreadCounts(QStringLiteral("channel")));
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));
    }

    void crtUsesRootMentionCount()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5, true, 3, 0, true, false);

        synchronize(tracker, 1000, 5, 5, true, true);
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        synchronize(tracker, 1000, 5, 5, true, false);
        QVERIFY(tracker.hasMention(QStringLiteral("channel")));
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void membershipResponsePreservesRuntimeActivity()
    {
        ChannelActivityTracker tracker;
        tracker.recordPost(QStringLiteral("channel"), 3000, false, false, true);

        // Simulate a membership response that was requested before the
        // websocket post and therefore contains older server state.
        setMembership(tracker, 1000, 5, 5, true, 0, 0, true, false);
        synchronize(tracker, 2000, 5, 5, true, false);

        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(tracker.hasMention(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(3000));
    }

    void serverCursorDoesNotAdvanceOnOptimisticLocalRead()
    {
        ChannelActivityTracker tracker;
        const QString channel = QStringLiteral("channel");
        QCOMPARE(tracker.lastServerViewedTime(channel), uint64_t(0));
        setMembership(tracker, 1000, 5, 5);
        QCOMPARE(tracker.lastServerViewedTime(channel), uint64_t(1000));

        // An unacknowledged local view changes the UI state but cannot yet
        // silence the desktop notification through the server read boundary.
        tracker.recordViewed(channel, 4000, 7, 7, true);
        QCOMPARE(tracker.lastViewedTime(channel), uint64_t(4000));
        QCOMPARE(tracker.lastServerViewedTime(channel), uint64_t(1000));

        // A membership snapshot or server channel_viewed acknowledges progress.
        setMembership(tracker, 2500, 6, 6);
        QCOMPARE(tracker.lastServerViewedTime(channel), uint64_t(2500));
        tracker.recordServerViewed(channel, 4000, 7, 7, true);
        QCOMPARE(tracker.lastServerViewedTime(channel), uint64_t(4000));

        // A stale membership response must not regress the confirmed cursor.
        setMembership(tracker, 2000, 5, 5);
        QCOMPARE(tracker.lastServerViewedTime(channel), uint64_t(4000));

        // Explicit server-backed mark-as-unread legitimately rewinds it.
        tracker.markUnread(channel, 2999, 5, 5, true, 0, 0, true);
        QCOMPARE(tracker.lastServerViewedTime(channel), uint64_t(2999));
        tracker.recordViewed(channel, 4500, 7, 7, true);
        QCOMPARE(tracker.lastServerViewedTime(channel), uint64_t(2999));
    }

    void staleMembershipCannotRegressReadState()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 1000, 5, 5, true, false);

        // A newer local/websocket acknowledgement wins before an older REST
        // membership request completes.
        tracker.recordViewed(QStringLiteral("channel"), 5000, 12, 12, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));

        // The stale response carries both older counters and an old mention.
        // Neither may resurrect unread state after the newer read watermark.
        setMembership(tracker, 2000, 7, 7, true, 3, 3, true, false);
        synchronize(tracker, 5000, 12, 12, true, false);

        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(5000));
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));
    }

    void staleMembershipCountsAreRejectedAtEqualWatermark()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 1000, 5, 5, true, false);
        tracker.recordViewed(QStringLiteral("channel"), 5000, 12, 12, true);

        // Timestamp equality is insufficient to establish freshness. A lower
        // read counter proves that this snapshot predates the acknowledged read.
        setMembership(tracker, 5000, 8, 8, true, 2, 2, true, false);
        synchronize(tracker, 5000, 12, 12, true, false);

        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(5000));
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));
    }

    void viewingChannelClearsServerAndRuntimeUnread()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 2000, 7, 7, true, false);
        tracker.recordPost(QStringLiteral("channel"), 2500, false, false, true);

        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(tracker.hasMention(QStringLiteral("channel")));

        tracker.recordViewed(QStringLiteral("channel"), 2500, 8, 8, true);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));
    }

    void reopeningReadChannelDoesNotAdvanceRecentTime()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 2000, 7, 7);
        synchronize(tracker, 2000, 7, 7, true, false);

        QCOMPARE(tracker.recentTime(QStringLiteral("channel")), uint64_t(2000));

        // Viewing the same already-read content again uses the channel's last
        // post timestamp, not wall-clock time, so recency is stable.
        tracker.recordViewed(QStringLiteral("channel"), 2000, 7, 7, true);
        QCOMPARE(tracker.recentTime(QStringLiteral("channel")), uint64_t(2000));
    }

    void recentTimeIncludesMattermostRecencyPreferences()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        tracker.setRecencyTimes(QStringLiteral("channel"), 2000, 3000);

        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(1000));
        QCOMPARE(tracker.recentTime(QStringLiteral("channel")), uint64_t(3000));

        tracker.recordViewed(QStringLiteral("channel"), 2500, 5, 5, true);
        QCOMPARE(tracker.recentTime(QStringLiteral("channel")), uint64_t(3000));
    }

    void mutedChannelOnlyRequiresAttentionForMention()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5, true, 0, 0, true, true);
        synchronize(tracker, 2000, 7, 7, true, false);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        tracker.setMentioned(QStringLiteral("channel"), true);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));

        tracker.setMentioned(QStringLiteral("channel"), false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        tracker.setMuted(QStringLiteral("channel"), false);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void threadReplyBelongsToChannelWhenCrtIsOff()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 1000, 5, 5, true, false);

        tracker.recordPost(QStringLiteral("channel"), 2000, false, true, false);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(2000));
    }

    void threadReplyStaysOutOfParentChannelWhenCrtIsOn()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 1000, 5, 5, true, true);

        tracker.recordPost(QStringLiteral("channel"), 2000, false, true, false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(2000));

        // A mention in a CRT reply belongs to the followed thread's unread
        // state, not to the parent channel's mention_count_root.
        tracker.recordPost(QStringLiteral("channel"), 2100, false, true, true);
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
    }

    void explicitMarkUnreadCanMoveWatermarkBackwards()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 5000, 12, 8);
        synchronize(tracker, 5000, 12, 8, true, false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        tracker.markUnread(QStringLiteral("channel"), 2999,
                           7, 5, true, 0, 0, true);
        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(2999));
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));

        tracker.recordViewed(QStringLiteral("channel"), 5000, 12, 8, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QCOMPARE(tracker.lastViewedTime(QStringLiteral("channel")), uint64_t(5000));
    }

    void ownPostDoesNotCreateUnreadState()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 1000, 5, 5, true, false);

        // Even if the websocket metadata says the current user is mentioned,
        // their own post cannot create unread/mention attention for themselves.
        tracker.recordPost(QStringLiteral("channel"), 2500, true, false, true);

        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));
        QCOMPARE(tracker.activityTime(QStringLiteral("channel")), uint64_t(2500));
    }

    void ownPostDoesNotReappearFromStaleServerCounters()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 1000, 5, 5, true, false);

        tracker.recordPost(QStringLiteral("channel"), 2500, true, false, false);

        // Channel totals can advance before the membership msg_count does.
        // The one-message gap is exactly the locally observed own post.
        synchronize(tracker, 2500, 6, 6, true, false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        // A stale membership response must not drop that local knowledge.
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 2500, 6, 6, true, false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        // Once the membership counter catches up, the credit is consumed. A
        // subsequent unexplained server gap must therefore become unread.
        setMembership(tracker, 2500, 6, 6);
        synchronize(tracker, 3000, 7, 7, true, false);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void ownRootPostDoesNotReappearInCrtRootCounters()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 20, 5);
        synchronize(tracker, 1000, 20, 5, true, true);
        QVERIFY(tracker.usesRootUnreadCounts(QStringLiteral("channel")));

        tracker.recordPost(QStringLiteral("channel"), 2000, true, false, false);
        synchronize(tracker, 2000, 21, 6, true, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        setMembership(tracker, 2000, 21, 6);
        synchronize(tracker, 3000, 22, 7, true, true);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void ownReplyCreditSurvivesCrtFallback()
    {
        ChannelActivityTracker tracker;
        setMembership(tracker, 1000, 5, 5);
        synchronize(tracker, 1000, 5, 5, true, true);
        QVERIFY(tracker.usesRootUnreadCounts(QStringLiteral("channel")));

        tracker.recordPost(QStringLiteral("channel"), 2000, true, true, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));
        QVERIFY(!tracker.hasMention(QStringLiteral("channel")));

        // Replies do not advance the root total while CRT is active.
        synchronize(tracker, 2000, 6, 5, true, true);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        // If root counters disappear, the all-message fallback now includes the
        // reply. The own-post credit must move with that semantic domain.
        synchronize(tracker, 2000, 6, 0, false, true);
        QVERIFY(!tracker.usesRootUnreadCounts(QStringLiteral("channel")));
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        setMembership(tracker, 2000, 6, 5);
        synchronize(tracker, 3000, 7, 0, false, true);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }

    void ownPostBeforeFirstMembershipDoesNotMaskLaterUnread()
    {
        ChannelActivityTracker tracker;

        // Without an absolute read-count baseline an own post cannot safely be
        // carried as count credit: the first membership may already include it.
        tracker.recordPost(QStringLiteral("channel"), 2000, true, false, false);
        setMembership(tracker, 2000, 6, 6);
        synchronize(tracker, 2000, 6, 6, true, false);
        QVERIFY(!tracker.isUnread(QStringLiteral("channel")));

        // A later server-only gap is real unread state and must not be hidden by
        // an unanchored credit from before membership initialization.
        synchronize(tracker, 3000, 7, 7, true, false);
        QVERIFY(tracker.isUnread(QStringLiteral("channel")));
    }
};

QTEST_MAIN(ChannelActivityTrackerTest)

#include "ChannelActivityTrackerTest.moc"
