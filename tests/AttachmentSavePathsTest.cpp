#include <QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "chat-area/post/attachments/AttachmentSavePaths.h"

using namespace Mattermost;

class AttachmentSavePathsTest : public QObject
{
    Q_OBJECT

private slots:
    void preventsPathTraversalAndWindowsDeviceNames()
    {
        using AttachmentSavePaths::safeName;
        QCOMPARE(safeName(QStringLiteral("../../report.pdf")),
                 QStringLiteral("report.pdf"));
        QCOMPARE(safeName(QStringLiteral("folder\\secrets.txt")),
                 QStringLiteral("secrets.txt"));
        QCOMPARE(safeName(QStringLiteral("a:b?c*.txt")),
                 QStringLiteral("a_b_c_.txt"));
        QCOMPARE(safeName(QStringLiteral("CON.txt")),
                 QStringLiteral("_CON.txt"));
        QCOMPARE(safeName(QStringLiteral("LPT1")),
                 QStringLiteral("_LPT1"));
        QCOMPARE(safeName(QStringLiteral("  .  ")),
                 QStringLiteral("attachment"));
        QCOMPARE(safeName(QStringLiteral("image.png")),
                 QStringLiteral("image.png"));
    }

    void duplicateNamesNeverOverwrite()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QDir folder(temp.path());

        QFile original(folder.filePath(QStringLiteral("record.txt")));
        QVERIFY(original.open(QIODevice::WriteOnly));
        QCOMPARE(original.write("original"), qint64(8));
        original.close();

        const QString second = AttachmentSavePaths::unusedPath(
            folder, QStringLiteral("record.txt"));
        QCOMPARE(QFileInfo(second).fileName(), QStringLiteral("record (1).txt"));

        QSet<QString> reserved;
        reserved.insert(QStringLiteral("RECORD (1).TXT"));
        const QString third = AttachmentSavePaths::unusedPath(
            folder, QStringLiteral("../record.txt"), reserved);
        QCOMPARE(QFileInfo(third).fileName(), QStringLiteral("record (2).txt"));
        QVERIFY(!QFileInfo::exists(third));

        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("original"));
    }
};

QTEST_GUILESS_MAIN(AttachmentSavePathsTest)
#include "AttachmentSavePathsTest.moc"
