#include <QtTest>

#include "backend/ChannelUnreadPostSelection.h"
#include "channel-tree/AttentionActivationPolicy.h"
#include "channel-tree/FollowingActivationPolicy.h"
#include "channel-tree/FollowingPresentation.h"
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
