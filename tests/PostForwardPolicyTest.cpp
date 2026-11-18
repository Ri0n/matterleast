#include <QtTest>

#include <QJsonArray>
#include <QJsonObject>

#include "chat-area/post/PostForwardPolicy.h"
#include "backend/types/BackendChannel.h"

using namespace Mattermost;

class PostForwardPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void permissionBoundary_data()
    {
        QTest::addColumn<int>("sourceType");
        QTest::addColumn<QString>("sourceId");
        QTest::addColumn<QString>("destinationId");
        QTest::addColumn<bool>("allowed");

        QTest::newRow("public-to-other")
            << int(BackendChannel::publicChannel) << QString("source")
            << QString("another") << true;
        QTest::newRow("public-to-self")
            << int(BackendChannel::publicChannel) << QString("source")
            << QString("source") << true;
        for (int type : { int(BackendChannel::privateChannel),
                          int(BackendChannel::directChannel),
                          int(BackendChannel::groupChannel) }) {
            const QByteArray name = QByteArray::number(type);
            QTest::newRow((name + "-to-self").constData())
                << type << QString("source") << QString("source") << true;
            QTest::newRow((name + "-to-other").constData())
                << type << QString("source") << QString("another") << false;
        }
        QTest::newRow("unknown-is-not-public")
            << int(BackendChannel::unknown) << QString("source")
            << QString("another") << false;
        QTest::newRow("empty-source")
            << int(BackendChannel::publicChannel) << QString()
            << QString("another") << false;
        QTest::newRow("empty-destination")
            << int(BackendChannel::publicChannel) << QString("source")
            << QString() << false;
    }

    void permissionBoundary()
    {
        QFETCH(int, sourceType);
        QFETCH(QString, sourceId);
        QFETCH(QString, destinationId);
        QFETCH(bool, allowed);
        QCOMPARE(canForwardPostTo(sourceType, sourceId, destinationId), allowed);
    }

    void canonicalPermalinkMatchesServerRecognizer()
    {
        const QString id = QStringLiteral("abcdefghijklmnopqrstuvwx12");
        QCOMPARE(id.size(), 26);
        QCOMPARE(nativeForwardPermalink(
                     QStringLiteral("https://canonical.example.org/mattermost/"),
                     QStringLiteral("my-team"), id),
                 QStringLiteral("https://canonical.example.org/mattermost/"
                                "my-team/pl/") + id);
        QCOMPARE(nativeForwardPermalink(QStringLiteral("https://official.example"),
                                        QStringLiteral("team"), id),
                 QStringLiteral("https://official.example/team/pl/") + id);
        // An alternate connection URL and /_redirect/pl/id are not used.
        QVERIFY(nativeForwardPermalink(QStringLiteral("not-a-server"),
                                      QStringLiteral("team"), id).isEmpty());
        QVERIFY(nativeForwardPermalink(QStringLiteral("https://server"),
                                      QStringLiteral("team"), QStringLiteral("short")).isEmpty());
        QVERIFY(nativeForwardPermalink(QStringLiteral("https://server"),
                                      QStringLiteral("Invalid Team"), id).isEmpty());
    }

    void serverAuthorizedPreviewHidesOnlyItsOwnPermalink()
    {
        const QString id = QStringLiteral("abcdefghijklmnopqrstuvwx12");
        const QString url = QStringLiteral("https://canonical.example/team/pl/") + id;
        const QJsonArray previews{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("permalink")},
            {QStringLiteral("data"), QJsonObject{
                {QStringLiteral("post_id"), id},
                {QStringLiteral("post"), QJsonObject{
                    {QStringLiteral("id"), id},
                    {QStringLiteral("message"), QStringLiteral("original text")},
                }},
            }},
        }};
        QCOMPARE(textWithPermalinkPreview(url, previews), QString());
        QCOMPARE(textWithPermalinkPreview(QStringLiteral("Comment\n") + url, previews),
                 QStringLiteral("Comment"));
        QCOMPARE(textWithPermalinkPreview(QStringLiteral("Comment\n") + url, {}),
                 QStringLiteral("Comment\n") + url);
        const QJsonArray withheld{QJsonObject{
            {QStringLiteral("type"), QStringLiteral("permalink")},
            {QStringLiteral("data"), QJsonObject{{QStringLiteral("post_id"), id}}},
        }};
        QCOMPARE(textWithPermalinkPreview(url, withheld), url);
        QCOMPARE(textWithPermalinkPreview(
                     QStringLiteral("https://canonical.example/team/pl/other"), previews),
                 QStringLiteral("https://canonical.example/team/pl/other"));
    }

    void incomingStandaloneForwardMetadata()
    {
        const QJsonObject received{
            {QStringLiteral("isForwarded"), true},
            {QStringLiteral("originalPostId"), QStringLiteral("source-post")},
            {QStringLiteral("originalAuthorId"), QStringLiteral("source-author")},
            {QStringLiteral("current_team_id"), QStringLiteral("team")},
        };
        const ForwardedCopyInfo info = forwardedCopyInfo(received);
        QVERIFY(info.isForwarded);
        QCOMPARE(info.originalPostId, QStringLiteral("source-post"));
        QCOMPARE(info.originalAuthorId, QStringLiteral("source-author"));

        const ForwardedCopyInfo missing = forwardedCopyInfo(QJsonObject{});
        QVERIFY(!missing.isForwarded);
        const ForwardedCopyInfo malformed = forwardedCopyInfo(QJsonObject{
            {QStringLiteral("isForwarded"), QStringLiteral("true")},
            {QStringLiteral("originalAuthorId"), QStringLiteral("fake")},
        });
        QVERIFY(!malformed.isForwarded);
        const ForwardedCopyInfo partial = forwardedCopyInfo(QJsonObject{
            {QStringLiteral("isForwarded"), true},
        });
        QVERIFY(partial.isForwarded);
        QVERIFY(partial.originalPostId.isEmpty());
        QVERIFY(partial.originalAuthorId.isEmpty());
    }

    void standaloneCopyPayload()
    {
        QCOMPARE(forwardedCopyText({}, QStringLiteral("Original **Markdown**")),
                 QStringLiteral("Original **Markdown**"));
        QCOMPARE(forwardedCopyText(QStringLiteral("Comment"),
                                   QStringLiteral("Original")),
                 QStringLiteral("Comment\n\nOriginal"));
        QCOMPARE(forwardedCopyText(QStringLiteral("Comment"), {}), QString());
        const QJsonObject props = forwardedCopyProps(
            QStringLiteral("original-id"),
            QStringLiteral("author-id"),
            QStringLiteral("team-id"));
        QCOMPARE(props.value(QStringLiteral("isForwarded")).toBool(), true);
        QCOMPARE(props.value(QStringLiteral("originalPostId")).toString(),
                 QStringLiteral("original-id"));
        QCOMPARE(props.value(QStringLiteral("originalAuthorId")).toString(),
                 QStringLiteral("author-id"));
        QCOMPARE(props.value(QStringLiteral("current_team_id")).toString(),
                 QStringLiteral("team-id"));
        QVERIFY(!forwardedCopyText({}, QStringLiteral("Original"))
                    .contains(QStringLiteral("/pl/")));
    }

    void forwardingPayload()
    {
        const QString link("https://canonical.example/team/pl/abcdefghijklmnopqrstuvwxyz");
        QCOMPARE(forwardedPostText({}, link), link);
        QCOMPARE(forwardedPostText("  \t  ", link), link);
        QCOMPARE(forwardedPostText("Context", link),
                 QString("Context\n") + link);
        QCOMPARE(forwardedPostText("Context\nwith two lines", link),
                 QString("Context\nwith two lines\n") + link);
        QCOMPARE(forwardedPostText("Context", {}),
                 QString());
    }
};

QTEST_MAIN(PostForwardPolicyTest)
#include "PostForwardPolicyTest.moc"
