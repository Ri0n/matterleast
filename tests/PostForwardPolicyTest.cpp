#include <QtTest>

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

    void forwardingPayload()
    {
        const QString link("https://server/_redirect/pl/postid");
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
