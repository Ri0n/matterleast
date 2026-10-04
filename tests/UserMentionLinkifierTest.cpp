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

QTEST_MAIN(UserMentionLinkifierTest)
#include "UserMentionLinkifierTest.moc"
