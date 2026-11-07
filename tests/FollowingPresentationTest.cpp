#include <QtTest>

#include <algorithm>

#include "backend/ChannelUnreadPostSelection.h"
#include "channel-tree/AttentionActivationPolicy.h"
#include "channel-tree/FollowingActivationPolicy.h"
#include "channel-tree/FollowingPresentation.h"
#include "channel-tree/UnreadProjectionPolicy.h"
#include "channel-tree/SidebarUnreadPolicy.h"
#include "channel-tree/VirtualDestinationBlock.h"

using namespace Mattermost;

class FollowingPresentationTest : public QObject
{
    Q_OBJECT

private slots:
    void threadLabelHasNoTypeGlyphPrefix()
    {
        QCOMPARE(followingThreadLabel(QStringLiteral("Town Square"),
                                      QStringLiteral("Hello there")),
                 QStringLiteral("Town Square \u2014 Hello there"));

        const QString label = followingThreadLabel(
            QStringLiteral("Town Square"),
            QStringLiteral("Hello there"));
        QVERIFY(!label.startsWith(QStringLiteral("@ ")));
        QVERIFY(!label.startsWith(QStringLiteral("\u21aa ")));
    }

    void emptySnippetUsesOnlyChannelName()
    {
        QCOMPARE(followingThreadLabel(QStringLiteral("Town Square"),
                                      QStringLiteral("   \n\t  ")),
                 QStringLiteral("Town Square"));
    }

    void tooltipUsesOnlyFirstMessageLine()
    {
        QCOMPARE(followingMessageToolTip(
                     QStringLiteral("first line\nsecond line\nthird line")),
                 QStringLiteral("first line"));

        const QString longLine(FollowingThreadSnippetLength + 20, QLatin1Char('x'));
        const QString tooltip = followingMessageToolTip(longLine + QStringLiteral("\nignored"));
        QCOMPARE(tooltip.size(), FollowingThreadSnippetLength);
        QVERIFY(tooltip.endsWith(QChar(0x2026)));
        QVERIFY(!tooltip.contains(QLatin1Char('\n')));
    }

    void unreadConversationNavigationNeverTargetsCrtReplies()
    {
        // Synthetic protocol fixtures, deliberately unrelated to any real
        // conversation or diagnostic log. The unread endpoint orders newest
        // first, so the old implementation picked the final reply.
        const QJsonObject response {
            {"order", QJsonArray {QStringLiteral("root-new"),
                                  QStringLiteral("reply-old")}},
            {"posts", QJsonObject {
                {"root-new", QJsonObject {{"root_id", ""}}},
                {"reply-old", QJsonObject {{"root_id", "thread-root"}}}
            }}
        };

        QCOMPARE(selectChannelUnreadPostId(response, false),
                 QStringLiteral("reply-old"));
        QCOMPARE(selectChannelUnreadPostId(response, true),
                 QStringLiteral("root-new"));

        // With no verified root in the server's short unread window,
        // navigate to the parent DM tail rather than the unrelated thread.
        const QJsonObject onlyReply {
            {"order", QJsonArray {QStringLiteral("reply-only")}},
            {"posts", QJsonObject {
                {"reply-only", QJsonObject {{"root_id", "thread-root"}}}
            }}
        };
        QVERIFY(selectChannelUnreadPostId(onlyReply, true).isEmpty());
        QCOMPARE(selectChannelUnreadPostId(onlyReply, false),
                 QStringLiteral("reply-only"));

        const QJsonObject unknownMetadata {
            {"order", QJsonArray {QStringLiteral("unknown")}},
            {"posts", QJsonObject {{"unknown", QJsonObject {{"id", "unknown"}}}}}
        };
        QVERIFY(selectChannelUnreadPostId(unknownMetadata, true).isEmpty());

        // A cached FirstUnread cursor must respect the same root boundary.
        QVERIFY(isConversationTargetCompatible(true, true, QString()));
        QVERIFY(!isConversationTargetCompatible(
            true, true, QStringLiteral("thread-root")));
        QVERIFY(!isConversationTargetCompatible(true, false, QString()));
        QVERIFY(isConversationTargetCompatible(
            false, true, QStringLiteral("thread-root")));
    }

    void activationFlashesOnlyUnreadAttentionPosts()
    {
        FollowingModel::Entry conversation;
        conversation.channelId = QStringLiteral("dm");
        conversation.unread = true;
        AttentionViewportPost visible;
        visible.channelId = QStringLiteral("dm");
        visible.postId = QStringLiteral("new");
        visible.createAt = 150;
        visible.channelLastViewedAt = 100;

        QVERIFY(shouldFlashAttentionPost(conversation, visible));
        visible.createAt = 90;
        QVERIFY(!shouldFlashAttentionPost(conversation, visible));
        visible.createAt = 150;
        visible.channelId = QStringLiteral("other");
        QVERIFY(!shouldFlashAttentionPost(conversation, visible));
        visible.channelId = QStringLiteral("dm");
        visible.postRootId = QStringLiteral("thread");
        QVERIFY(!shouldFlashAttentionPost(conversation, visible));
        visible.postRootId.clear();

        conversation.resumeState = FollowingModel::ResumeState::FirstUnread;
        conversation.firstUnreadPostId = QStringLiteral("first-new");
        QVERIFY(!shouldFlashAttentionPost(conversation, visible));
        visible.firstUnreadOrLater = true;
        QVERIFY(shouldFlashAttentionPost(conversation, visible));

        conversation.readThroughPostId = QStringLiteral("read");
        conversation.readThroughCreateAt = 160;
        QVERIFY(!shouldFlashAttentionPost(conversation, visible));
        visible.createAt = 161;
        QVERIFY(shouldFlashAttentionPost(conversation, visible));

        conversation.muted = true;
        QVERIFY(!shouldFlashAttentionPost(conversation, visible));
    }

    void activationRespectsThreadAndSyntheticMentionIdentity()
    {
        FollowingModel::Entry thread;
        thread.kind = FollowingModel::Kind::Thread;
        thread.channelId = QStringLiteral("town");
        thread.threadId = QStringLiteral("root");
        thread.unreadReplies = 3;
        thread.lastViewedAt = 100;

        AttentionViewportPost visible;
        visible.channelId = QStringLiteral("town");
        visible.threadId = QStringLiteral("root");
        visible.postId = QStringLiteral("reply");
        visible.postRootId = QStringLiteral("root");
        visible.createAt = 150;
        QVERIFY(shouldFlashAttentionPost(thread, visible));

        visible.postRootId.clear();
        visible.postId = QStringLiteral("root");
        QVERIFY(!shouldFlashAttentionPost(thread, visible));
        visible.postRootId = QStringLiteral("root");
        visible.threadId.clear();
        QVERIFY(!shouldFlashAttentionPost(thread, visible));

        thread.synthetic = true;
        thread.unreadReplies = 0;
        thread.unreadMentions = 1;
        visible.postRootId.clear();
        QVERIFY(shouldFlashAttentionPost(thread, visible));
        visible.postId = QStringLiteral("another");
        QVERIFY(!shouldFlashAttentionPost(thread, visible));
        visible.postId = QStringLiteral("root");
        visible.threadId = QStringLiteral("root");
        QVERIFY(!shouldFlashAttentionPost(thread, visible));

        thread.synthetic = false;
        thread.muted = true;
        visible.threadId = QStringLiteral("root");
        visible.postId = QStringLiteral("reply");
        visible.postRootId = QStringLiteral("root");
        QVERIFY(shouldFlashAttentionPost(thread, visible));
    }

    void unreadViewPreservesFollowingRows()
    {
        UnreadViewFilters filters;
        QVERIFY(filters.following);
        QVERIFY(filters.channels);
        QVERIFY(!filters.muted);

        // Existing Following includes read followed threads and synthetic
        // mentions, even when the enclosing channel is muted.
        QVERIFY(includeFollowingInUnread(true, true, filters));
        QVERIFY(includeFollowingInUnread(true, false, filters));
        QVERIFY(includeFollowingInUnread(false, false, filters));
        QVERIFY(!includeFollowingInUnread(false, true, filters));

        filters.following = false;
        QVERIFY(!includeFollowingInUnread(true, false, filters));
        QVERIFY(!includeFollowingInUnread(false, false, filters));
    }

    void unreadViewChannelsExcludeDirectMessagesAndReadChannels()
    {
        UnreadViewFilters filters;
        // Only joined public/private channels with server unread state.
        QVERIFY(includeChannelInUnread(true, true, true, false, false, filters));
        QVERIFY(!includeChannelInUnread(false, true, true, false, false, filters));
        QVERIFY(!includeChannelInUnread(true, false, true, false, false, filters));
        QVERIFY(!includeChannelInUnread(true, true, false, false, false, filters));
        // A channel requiring explicit personal attention stays in Following
        // / Attention, rather than becoming an ordinary Channels row.
        QVERIFY(!includeChannelInUnread(true, true, true, true, false, filters));

        filters.channels = false;
        QVERIFY(!includeChannelInUnread(true, true, true, false, false, filters));
    }

    void unreadViewMutedIsOnlyAnInclusionModifier()
    {
        UnreadViewFilters filters;
        QVERIFY(!includeChannelInUnread(true, true, true, false, true, filters));
        filters.muted = true;
        QVERIFY(includeChannelInUnread(true, true, true, false, true, filters));
        QVERIFY(includeFollowingInUnread(false, true, filters));

        // Muted cannot force read or untracked channels into the list.
        QVERIFY(!includeChannelInUnread(true, true, false, false, true, filters));
        QVERIFY(!includeChannelInUnread(true, false, true, false, true, filters));
        filters.channels = false;
        filters.following = false;
        // Muted is a third independent source: it can show muted channels
        // and DMs even with Following and Channels unchecked.
        QVERIFY(includeChannelInUnread(true, true, true, false, true, filters));
        QVERIFY(includeChannelInUnread(false, true, true, false, true, filters));
        QVERIFY(includeChannelInUnread(false, true, true, true, true, filters));
        QVERIFY(!includeChannelInUnread(true, true, true, false, false, filters));
        QVERIFY(!includeFollowingInUnread(true, true, filters));
    }

    void unreadViewSortsFollowingBeforeChannelsAndReadThreadsLast()
    {
        // The newest unread channel must not jump over an unread Following
        // thread. Muted and ordinary channels must interleave by timestamp.
        QVector<UnreadSortKey> keys {
            {unreadSortGroup(true, false), 9000, QStringLiteral("read-following")},
            {unreadSortGroup(false, true), 6000, QStringLiteral("channel-new")},
            {unreadSortGroup(false, true), 8000, QStringLiteral("muted-new")},
            {unreadSortGroup(true, true), 2000, QStringLiteral("following-old")},
            {unreadSortGroup(false, true), 4000, QStringLiteral("muted-old")},
            {unreadSortGroup(true, true), 3000, QStringLiteral("following-new")},
            {unreadSortGroup(true, false), 10000, QStringLiteral("read-following-new")}
        };
        std::sort(keys.begin(), keys.end(), unreadSortBefore);

        QStringList ordered;
        for (const UnreadSortKey& key : keys) {
            ordered << key.identity;
        }
        QCOMPARE(ordered, (QStringList {
            QStringLiteral("following-new"),
            QStringLiteral("following-old"),
            QStringLiteral("muted-new"),
            QStringLiteral("channel-new"),
            QStringLiteral("muted-old"),
            QStringLiteral("read-following-new"),
            QStringLiteral("read-following")
        }));
    }

    void unreadViewSortTiesAreStableAndSourceDriven()
    {
        const auto followedUnread = unreadSortGroup(true, true);
        const auto channelUnread = unreadSortGroup(false, true);
        const auto followedRead = unreadSortGroup(true, false);
        QCOMPARE(static_cast<int>(followedUnread),
                 static_cast<int>(UnreadSortGroup::UnreadFollowing));
        QCOMPARE(static_cast<int>(channelUnread),
                 static_cast<int>(UnreadSortGroup::UnreadChannel));
        QCOMPARE(static_cast<int>(followedRead),
                 static_cast<int>(UnreadSortGroup::ReadFollowing));

        const UnreadSortKey a {channelUnread, 4000, QStringLiteral("c:a")};
        const UnreadSortKey b {channelUnread, 4000, QStringLiteral("c:b")};
        QVERIFY(unreadSortBefore(a, b));
        QVERIFY(!unreadSortBefore(b, a));
        QVERIFY(!unreadSortBefore(a, a));

        // The group is authoritative even if a read followed thread has a
        // more recent timestamp than every currently unread channel.
        QVERIFY(unreadSortBefore(
            {channelUnread, 1, QStringLiteral("channel")},
            {followedRead, 999999, QStringLiteral("thread")}));
        QVERIFY(unreadSortBefore(
            {followedUnread, 1, QStringLiteral("thread")},
            {channelUnread, 999999, QStringLiteral("muted")}));
    }

    void mutedAttentionKeepsExplicitlyFollowedThreads()
    {
        QVERIFY(attentionMuteAllowsEntry(false, false));
        QVERIFY(attentionMuteAllowsEntry(false, true));
        QVERIFY(!attentionMuteAllowsEntry(true, false));
        QVERIFY(attentionMuteAllowsEntry(true, true));
    }

    void repeatedOpenConversationActivationPreservesViewport()
    {
        QVERIFY(shouldPreserveRepeatedConversationActivation(
            false, QStringLiteral("channel"), QStringLiteral("channel"), true));
        QVERIFY(!shouldPreserveRepeatedConversationActivation(
            false, QStringLiteral("channel"), QStringLiteral("channel"), false));
        QVERIFY(!shouldPreserveRepeatedConversationActivation(
            false, QStringLiteral("channel"), QStringLiteral("other"), true));
        QVERIFY(!shouldPreserveRepeatedConversationActivation(
            true, QStringLiteral("channel"), QStringLiteral("channel"), true));
    }

    void textFilterCanTemporarilySuspendUnreadGate()
    {
        using SidebarUnreadPolicy::unreadGateActive;

        QVERIFY(!unreadGateActive(false, false, false));
        QVERIFY(!unreadGateActive(false, true, false));
        QVERIFY(unreadGateActive(true, false, true));
        QVERIFY(!unreadGateActive(true, true, true));
        QVERIFY(unreadGateActive(true, true, false));
    }

    void textFilterStillCountsAsAnActiveFilterWhenUnreadGateIsSuspended()
    {
        using SidebarUnreadPolicy::anyFilterActive;

        QVERIFY(!anyFilterActive(false, false, true));
        QVERIFY(anyFilterActive(false, true, true));
        QVERIFY(anyFilterActive(true, false, true));
        QVERIFY(anyFilterActive(true, true, true));
        QVERIFY(anyFilterActive(true, true, false));
    }

    void leadingDestinationsIgnoreUnreadGateButRespectTextFilter()
    {
        using SidebarUnreadPolicy::virtualDestinationVisible;

        QVERIFY(virtualDestinationVisible(false, false));
        QVERIFY(virtualDestinationVisible(false, true));
        QVERIFY(!virtualDestinationVisible(true, false));
        QVERIFY(virtualDestinationVisible(true, true));
    }

    void leadingDestinationOrderIsStableAndComplete()
    {
        QCOMPARE(VirtualDestinationBlock::size(), 4);
        QCOMPARE(VirtualDestinationBlock::Destinations.at(0),
                 static_cast<int>(SidebarItem::PersonalDestination));
        QCOMPARE(VirtualDestinationBlock::Destinations.at(1),
                 static_cast<int>(SidebarItem::SavedDestination));
        QCOMPARE(VirtualDestinationBlock::Destinations.at(2),
                 static_cast<int>(SidebarItem::DraftsDestination));
        QCOMPARE(VirtualDestinationBlock::Destinations.at(3),
                 static_cast<int>(SidebarItem::RecentMentionsDestination));

        for (const int destination : VirtualDestinationBlock::Destinations) {
            QVERIFY(VirtualDestinationBlock::contains(destination));
        }
    }

    void snippetRemainsCompact()
    {
        const QString message(FollowingThreadSnippetLength + 20, QLatin1Char('x'));
        const QString label = followingThreadLabel(
            QStringLiteral("Channel"),
            message);

        const QString snippet = label.section(QStringLiteral(" \u2014 "), 1);
        QCOMPARE(snippet.size(), FollowingThreadSnippetLength);
        QVERIFY(snippet.endsWith(QChar(0x2026)));
    }
};

QTEST_MAIN(FollowingPresentationTest)
#include "FollowingPresentationTest.moc"
