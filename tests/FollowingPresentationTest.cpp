#include <QtTest>

#include "channel-tree/FollowingActivationPolicy.h"
#include "channel-tree/FollowingPresentation.h"

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
