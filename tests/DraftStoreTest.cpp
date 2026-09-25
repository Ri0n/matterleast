#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "backend/DraftStore.h"

using namespace Mattermost;

class DraftStoreTest : public QObject
{
    Q_OBJECT

private slots:
    void roundTripsIndependentChannelAndThreadDrafts()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = QDir(temp.path()).filePath(QStringLiteral("drafts.json"));

        DraftEntry channel;
        channel.channelId = QStringLiteral("channel");
        channel.message = QString::fromUtf8("канальный черновик");
        channel.updateAt = 100;
        channel.dirty = true;
        channel.remotePresent = true;
        channel.syncRequested = true;

        DraftEntry thread;
        thread.channelId = QStringLiteral("channel");
        thread.rootId = QStringLiteral("root");
        thread.message = QStringLiteral("thread draft");
        thread.replyToPostId = QStringLiteral("quoted-post");
        thread.updateAt = 200;

        QVERIFY(DraftStore::save(path, {channel, thread}));

        bool ok = false;
        const QVector<DraftEntry> loaded = DraftStore::load(path, &ok);
        QVERIFY(ok);
        QCOMPARE(loaded.size(), 2);
        QCOMPARE(loaded.at(0).key(), channel.key());
        QCOMPARE(loaded.at(0).message, channel.message);
        QVERIFY(loaded.at(0).dirty);
        QVERIFY(loaded.at(0).remotePresent);
        QVERIFY(loaded.at(0).syncRequested);
        QCOMPARE(loaded.at(1).key(), thread.key());
        QCOMPARE(loaded.at(1).replyToPostId, thread.replyToPostId);
        QVERIFY(!loaded.at(1).dirty);
        QVERIFY(channel.key() != thread.key());
    }

    void roundTripsMultipleRecoveredDraftsForOneConversation()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = QDir(temp.path()).filePath(QStringLiteral("drafts.json"));

        DraftEntry first;
        first.channelId = QStringLiteral("channel");
        first.rootId = QStringLiteral("root");
        first.message = QStringLiteral("first unsent");
        first.attachmentPaths = QStringList {
            QStringLiteral("/tmp/first.png"),
            QStringLiteral("/tmp/second.pdf"),
        };
        first.recoveryId = QStringLiteral("pending-1");
        first.updateAt = 100;

        DraftEntry second = first;
        second.message = QStringLiteral("second unsent");
        second.recoveryId = QStringLiteral("pending-2");
        second.updateAt = 200;

        QVERIFY(first.isRecovered());
        QVERIFY(second.isRecovered());
        QVERIFY(first.key() != second.key());
        QVERIFY(DraftStore::save(path, {first, second}));

        bool ok = false;
        const QVector<DraftEntry> loaded = DraftStore::load(path, &ok);
        QVERIFY(ok);
        QCOMPARE(loaded.size(), 2);
        QCOMPARE(loaded.at(0).recoveryId, first.recoveryId);
        QCOMPARE(loaded.at(0).attachmentPaths, first.attachmentPaths);
        QCOMPARE(loaded.at(1).recoveryId, second.recoveryId);
        QCOMPARE(loaded.at(1).attachmentPaths, second.attachmentPaths);
        QVERIFY(loaded.at(0).key() != loaded.at(1).key());
    }

    void preservesDeletionTombstone()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = QDir(temp.path()).filePath(QStringLiteral("drafts.json"));

        DraftEntry tombstone;
        tombstone.channelId = QStringLiteral("channel");
        tombstone.rootId = QStringLiteral("root");
        tombstone.updateAt = 300;
        tombstone.dirty = true;
        tombstone.deleted = true;
        tombstone.remotePresent = true;
        tombstone.syncRequested = true;

        QVERIFY(DraftStore::save(path, {tombstone}));

        bool ok = false;
        const QVector<DraftEntry> loaded = DraftStore::load(path, &ok);
        QVERIFY(ok);
        QCOMPARE(loaded.size(), 1);
        QVERIFY(loaded.first().deleted);
        QVERIFY(loaded.first().dirty);
        QVERIFY(loaded.first().remotePresent);
        QVERIFY(loaded.first().syncRequested);
    }

    void rejectsCorruptFile()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = QDir(temp.path()).filePath(QStringLiteral("drafts.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("not-json"), qint64(8));
        file.close();

        bool ok = true;
        QVERIFY(DraftStore::load(path, &ok).isEmpty());
        QVERIFY(!ok);
    }
};

QTEST_APPLESS_MAIN(DraftStoreTest)
#include "DraftStoreTest.moc"
