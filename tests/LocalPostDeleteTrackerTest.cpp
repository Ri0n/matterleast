#include <QtTest>

#include <QJsonObject>
#include <QUrl>

#include "backend/Storage.h"
#include "backend/LocalPostDeleteTracker.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

using namespace Mattermost;

namespace {
const QString sharedPostId = QStringLiteral("same-id-on-two-servers");

QJsonObject author(const QString& username)
{
    return {
        {QStringLiteral("id"), username},
        {QStringLiteral("username"), username}
    };
}

QJsonObject ownPost(const QString& userId)
{
    return {
        {QStringLiteral("id"), sharedPostId},
        {QStringLiteral("channel_id"), QStringLiteral("same-channel-id")},
        {QStringLiteral("user_id"), userId},
        {QStringLiteral("message"), QStringLiteral("Fixture")},
        {QStringLiteral("create_at"), 1000000000000.0}
    };
}

QUrl deleteUrl()
{
    return QUrl(QStringLiteral(
        "https://example.test/api/v4/posts/") + sharedPostId);
}
} // namespace

class LocalPostDeleteTrackerTest final : public QObject
{
    Q_OBJECT
private slots:
    void unrelatedEndpointsAreNotTracked()
    {
        LocalPostDeleteTracker tracker;
        tracker.noteRequest(QUrl(QStringLiteral(
            "https://example.test/api/v4/posts/other/patch")));
        tracker.noteRequest(QUrl(QStringLiteral(
            "https://example.test/api/v4/channels/same-id-on-two-servers")));
        QVERIFY(!tracker.take(sharedPostId));
        tracker.noteRequest(deleteUrl());
        QVERIFY(tracker.take(sharedPostId));
        QVERIFY(!tracker.take(sharedPostId));
    }

    void localAndRemoteDeletesOfSameIdHaveIndependentSemantics()
    {
        Storage one;
        Storage two;
        one.addUser(author(QStringLiteral("user-one")), true);
        two.addUser(author(QStringLiteral("user-two")), true);

        BackendChannel first(one, QJsonObject {
            {QStringLiteral("id"), QStringLiteral("same-channel-id")},
            {QStringLiteral("type"), QStringLiteral("O")}
        });
        BackendChannel second(two, QJsonObject {
            {QStringLiteral("id"), QStringLiteral("same-channel-id")},
            {QStringLiteral("type"), QStringLiteral("O")}
        });
        BackendPost* firstPost = first.addPost(ownPost(QStringLiteral("user-one")));
        BackendPost* secondPost = second.addPost(ownPost(QStringLiteral("user-two")));
        QVERIFY(firstPost);
        QVERIFY(secondPost);
        QVERIFY(firstPost->isOwnPost());
        QVERIFY(secondPost->isOwnPost());

        bool wasDeletedInFirstNotification = true;
        bool wasDeletedInSecondNotification = false;
        QSignalSpy firstSpy(&first, &BackendChannel::onPostDeleted);
        QSignalSpy secondSpy(&second, &BackendChannel::onPostDeleted);
        QObject::connect(&first, &BackendChannel::onPostDeleted,
                         &first, [&](const QString&) {
            wasDeletedInFirstNotification = firstPost->isDeleted;
        });
        QObject::connect(&second, &BackendChannel::onPostDeleted,
                         &second, [&](const QString&) {
            wasDeletedInSecondNotification = secondPost->isDeleted;
        });

        // A local DELETE from account one cannot turn the same post ID
        // received by account two into a locally-removed row.
        one.localPostDeleteTracker.noteRequest(deleteUrl());
        second.deletePost(sharedPostId);
        first.deletePost(sharedPostId);
        QCOMPARE(firstSpy.size(), 1);
        QCOMPARE(secondSpy.size(), 1);
        QVERIFY(!wasDeletedInFirstNotification);
        QVERIFY(wasDeletedInSecondNotification);
        QVERIFY(firstPost->isDeleted);
        QVERIFY(secondPost->isDeleted);
        QVERIFY(!one.localPostDeleteTracker.take(sharedPostId));
        QVERIFY(!two.localPostDeleteTracker.take(sharedPostId));
    }

    void resettingOneStorageLeavesOtherAccountsDeletionIntact()
    {
        Storage one;
        Storage two;
        one.localPostDeleteTracker.noteRequest(deleteUrl());
        two.localPostDeleteTracker.noteRequest(deleteUrl());

        one.reset();
        QVERIFY(!one.localPostDeleteTracker.take(sharedPostId));
        QVERIFY(two.localPostDeleteTracker.take(sharedPostId));
    }

    void failedDeleteClearsOnlyItsOwnAccount()
    {
        LocalPostDeleteTracker one;
        LocalPostDeleteTracker two;
        one.noteRequest(deleteUrl());
        two.noteRequest(deleteUrl());
        one.clearFailedRequest(deleteUrl());
        QVERIFY(!one.take(sharedPostId));
        QVERIFY(two.take(sharedPostId));
    }
};

QTEST_MAIN(LocalPostDeleteTrackerTest)
#include "LocalPostDeleteTrackerTest.moc"
