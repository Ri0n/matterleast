#include <QtTest/QtTest>

#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>

#include "chat-area/post/UserMentionLinkifier.h"

// Keep this unit test independent from matterleast-core while exercising the
// production implementation. The application builds the same .cpp normally via
// the recursive production source set.
#include "chat-area/post/UserMentionLinkifier.cpp"

using namespace Mattermost;

class UserMentionLinkifierTest final : public QObject
{
    Q_OBJECT

private slots:
    void channelReferenceLinksToTeamRoute();
    void channelReferenceWithoutTeamRemainsPlain();
    void channelReferenceInCodeRemainsPlain();
    void dottedUsernamesLinkImmediatelyWithoutTrailingPunctuation();
    void mentionFollowingExistingLinkIsNotSuppressed();
};

void UserMentionLinkifierTest::channelReferenceLinksToTeamRoute()
{
    QTextDocument document;
    document.setPlainText(QStringLiteral("See ~roadmap"));

    UserMentionLinkifier::linkify(document, {}, QStringLiteral("acme"));

    const QTextCursor link = document.find(QStringLiteral("~roadmap"));
    QVERIFY(!link.isNull());
    QVERIFY(link.charFormat().isAnchor());
    QCOMPARE(link.charFormat().anchorHref(), QStringLiteral("/acme/channels/roadmap"));
    QCOMPARE(link.charFormat().toolTip(), QStringLiteral("/acme/channels/roadmap"));
}

void UserMentionLinkifierTest::channelReferenceWithoutTeamRemainsPlain()
{
    QTextDocument document;
    document.setPlainText(QStringLiteral("See ~roadmap"));

    UserMentionLinkifier::linkify(document);

    const QTextCursor link = document.find(QStringLiteral("~roadmap"));
    QVERIFY(!link.isNull());
    QVERIFY(!link.charFormat().isAnchor());
}

void UserMentionLinkifierTest::channelReferenceInCodeRemainsPlain()
{
    QTextDocument document;
    document.setPlainText(QStringLiteral("Code: ~roadmap"));

    QTextCursor code = document.find(QStringLiteral("~roadmap"));
    QVERIFY(!code.isNull());
    QTextCharFormat codeFormat;
    codeFormat.setFontFixedPitch(true);
    code.mergeCharFormat(codeFormat);

    UserMentionLinkifier::linkify(document, {}, QStringLiteral("acme"));

    const QTextCursor link = document.find(QStringLiteral("~roadmap"));
    QVERIFY(!link.isNull());
    QVERIFY(!link.charFormat().isAnchor());
}

void UserMentionLinkifierTest::dottedUsernamesLinkImmediatelyWithoutTrailingPunctuation()
{
    QTextDocument document;
    document.setPlainText(QStringLiteral("From @login.name to @second.user."));

    UserMentionLinkifier::linkify(document);

    for (const QString& username : {QStringLiteral("login.name"),
                                    QStringLiteral("second.user")}) {
        QTextCursor cursor = document.find(QLatin1Char('@') + username);
        QVERIFY(!cursor.isNull());
        cursor.setPosition(cursor.selectionStart() + 2);
        QVERIFY(cursor.charFormat().isAnchor());
        QCOMPARE(cursor.charFormat().anchorHref(),
                 QStringLiteral("mattermost-user:///") + username);
    }

    QTextCursor punctuation(&document);
    punctuation.setPosition(document.toPlainText().length());
    QVERIFY(!punctuation.charFormat().isAnchor());
}

void UserMentionLinkifierTest::mentionFollowingExistingLinkIsNotSuppressed()
{
    QTextDocument document;
    document.setHtml(QStringLiteral(
        "<a href=\"https://example.org\">site</a> @login.name"));
    UserMentionLinkifier::linkify(document);

    QTextCursor cursor = document.find(QStringLiteral("@login.name"));
    QVERIFY(!cursor.isNull());
    cursor.setPosition(cursor.selectionStart() + 2);
    QVERIFY(cursor.charFormat().isAnchor());
    QCOMPARE(cursor.charFormat().anchorHref(),
             QStringLiteral("mattermost-user:///login.name"));
}

QTEST_MAIN(UserMentionLinkifierTest)
#include "UserMentionLinkifierTest.moc"
