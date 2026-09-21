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
        QCOMPARE(loaded.at(1).key(), thread.key());
        QCOMPARE(loaded.at(1).replyToPostId, thread.replyToPostId);
        QVERIFY(!loaded.at(1).dirty);
        QVERIFY(channel.key() != thread.key());
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

        QVERIFY(DraftStore::save(path, {tombstone}));

        bool ok = false;
        const QVector<DraftEntry> loaded = DraftStore::load(path, &ok);
        QVERIFY(ok);
        QCOMPARE(loaded.size(), 1);
        QVERIFY(loaded.first().deleted);
        QVERIFY(loaded.first().dirty);
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
