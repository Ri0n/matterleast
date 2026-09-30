#include <QtTest>

#include "channel-tree/FollowingActivationPolicy.h"
#include "channel-tree/FollowingPresentation.h"
#include "channel-tree/RecentMentionsIcon.h"
#include "channel-tree/SidebarUnreadPolicy.h"

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

    void unreadModeControlsFollowingVisibility()
    {
        using SidebarUnreadPolicy::followingVisible;

        QVERIFY(followingVisible(false, false));
        QVERIFY(followingVisible(false, true));
        QVERIFY(!followingVisible(true, false));
        QVERIFY(followingVisible(true, true));
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

    void recentMentionsIconProvidesNormalAndSelectedGlyphs()
    {
        QPalette palette;
        palette.setColor(QPalette::Text, QColor(10, 20, 30));
        palette.setColor(QPalette::HighlightedText, QColor(240, 230, 220));

        const QIcon icon = recentMentionsIcon(palette);
        QVERIFY(!icon.isNull());
        QVERIFY(!icon.pixmap(QSize(24, 24), QIcon::Normal).isNull());
        QVERIFY(!icon.pixmap(QSize(24, 24), QIcon::Selected).isNull());
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
