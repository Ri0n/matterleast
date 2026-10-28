#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include "update/WindowsZipInstaller.h"

namespace {
void u16(QByteArray& bytes, quint16 value)
{
    char data[2];
    qToLittleEndian(value, reinterpret_cast<uchar*>(data));
    bytes.append(data, sizeof(data));
}
void u32(QByteArray& bytes, quint32 value)
{
    char data[4];
    qToLittleEndian(value, reinterpret_cast<uchar*>(data));
    bytes.append(data, sizeof(data));
}

struct Entry {
    QByteArray name;
    quint32 uncompressed = 0;
    quint32 external = 0;
    quint16 flags = 0;
};

// Build a minimal, valid stored-entry ZIP using the wire format. This avoids
// depending on Python, PowerShell or system ZIP tools in the unit tests.
QByteArray zip(const QList<Entry>& entries)
{
    QByteArray output;
    struct Central { Entry entry; quint32 localOffset; };
    QList<Central> directory;
    for (const Entry& entry : entries) {
        const quint32 offset = quint32(output.size());
        u32(output, 0x04034b50);
        u16(output, 20); // needed version
        u16(output, entry.flags);
        u16(output, 0); // stored
        u16(output, 0); u16(output, 0); // DOS time and date
        u32(output, 0); // CRC32 for empty data
        u32(output, 0); // compressed bytes
        u32(output, entry.uncompressed);
        u16(output, quint16(entry.name.size()));
        u16(output, 0); // no extra fields
        output += entry.name;
        directory.append({entry, offset});
    }
    const quint32 centralOffset = quint32(output.size());
    for (const Central& central : directory) {
        const Entry& e = central.entry;
        u32(output, 0x02014b50);
        u16(output, 0x0314); // Unix creator
        u16(output, 20);
        u16(output, e.flags);
        u16(output, 0); u16(output, 0); u16(output, 0);
        u32(output, 0);
        u32(output, 0);
        u32(output, e.uncompressed);
        u16(output, quint16(e.name.size()));
        u16(output, 0); u16(output, 0); u16(output, 0); u16(output, 0);
        u32(output, e.external);
        u32(output, central.localOffset);
        output += e.name;
    }
    const quint32 centralSize = quint32(output.size()) - centralOffset;
    u32(output, 0x06054b50);
    u16(output, 0); u16(output, 0);
    u16(output, quint16(entries.size())); u16(output, quint16(entries.size()));
    u32(output, centralSize); u32(output, centralOffset);
    u16(output, 0);
    return output;
}

QList<Entry> regularFiles()
{
    return {
        { QByteArray("MatterLeast/MatterLeastLauncher.exe") },
        { QByteArray("MatterLeast/versions/v1.9/MatterLeast/bin/matterleast.exe") },
        { QByteArray("MatterLeast/versions/v1.9/MatterLeast/bin/MatterLeastUpdater.exe") },
        { QByteArray("MatterLeast/versions/v1.9/MatterLeast/bin/Qt6Core.dll") }
    };
}
}

class WindowsZipPolicyTest : public QObject
{
    Q_OBJECT
private slots:
    void acceptsCanonicalZip();
    void extractsCanonicalZipWithoutExternalTar();
    void blocksTraversal();
    void blocksLinks();
    void blocksCaseInsensitiveDuplicate();
    void blocksOversizedExtractedEntries();
    void blocksEncryptedEntries();
    void blocksMismatchedDirectory();
};

void WindowsZipPolicyTest::acceptsCanonicalZip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("valid.zip"));
    QFile output(path);
    QVERIFY(output.open(QIODevice::WriteOnly));
    const QByteArray bytes = zip(regularFiles());
    QCOMPARE(output.write(bytes), qint64(bytes.size()));
    output.close();
    QString error;
    QVERIFY2(Mattermost::auditWindowsUpdateZip(path, &error), qPrintable(error));
}

void WindowsZipPolicyTest::extractsCanonicalZipWithoutExternalTar()
{
#ifdef Q_OS_WIN
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString packagePath = temp.filePath(QStringLiteral("package.zip"));
    QFile output(packagePath);
    QVERIFY(output.open(QIODevice::WriteOnly));
    const QByteArray contents = zip(regularFiles());
    QCOMPARE(output.write(contents), qint64(contents.size()));
    output.close();
    QString error;
    QVERIFY2(Mattermost::prepareWindowsUpdateZip(
        packagePath, temp.path(), QStringLiteral("v1.9"), &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(temp.filePath(
        QStringLiteral("versions/v1.9/MatterLeast/bin/matterleast.exe"))));
    QVERIFY(QFileInfo::exists(temp.filePath(
        QStringLiteral("versions/v1.9/MatterLeast/bin/Qt6Core.dll"))));
#endif
}

void WindowsZipPolicyTest::blocksTraversal()
{
    auto entries = regularFiles();
    entries.append({QByteArray("MatterLeast/../escape.exe")});
    QTemporaryDir dir;
    QFile output(dir.filePath(QStringLiteral("traversal.zip")));
    QVERIFY(output.open(QIODevice::WriteOnly));
    output.write(zip(entries));
    output.close();
    QString error;
    QVERIFY(!Mattermost::auditWindowsUpdateZip(output.fileName(), &error));
}

void WindowsZipPolicyTest::blocksLinks()
{
    auto entries = regularFiles();
    entries.append({QByteArray("MatterLeast/versions/link"), 0,
                    quint32(0120000) << 16});
    QTemporaryDir dir;
    QFile output(dir.filePath(QStringLiteral("symlink.zip")));
    QVERIFY(output.open(QIODevice::WriteOnly));
    output.write(zip(entries)); output.close();
    QString error;
    QVERIFY(!Mattermost::auditWindowsUpdateZip(output.fileName(), &error));
}

void WindowsZipPolicyTest::blocksCaseInsensitiveDuplicate()
{
    auto entries = regularFiles();
    entries.append({QByteArray("MatterLeast/matterleastlauncher.exe")});
    QTemporaryDir dir;
    QFile output(dir.filePath(QStringLiteral("collision.zip")));
    QVERIFY(output.open(QIODevice::WriteOnly));
    output.write(zip(entries)); output.close();
    QString error;
    QVERIFY(!Mattermost::auditWindowsUpdateZip(output.fileName(), &error));
}

void WindowsZipPolicyTest::blocksOversizedExtractedEntries()
{
    auto entries = regularFiles();
    entries.append({QByteArray("MatterLeast/huge.txt"), 0xe0000000u});
    QTemporaryDir dir;
    QFile output(dir.filePath(QStringLiteral("bomb.zip")));
    QVERIFY(output.open(QIODevice::WriteOnly));
    output.write(zip(entries)); output.close();
    QString error;
    QVERIFY(!Mattermost::auditWindowsUpdateZip(output.fileName(), &error));
}

void WindowsZipPolicyTest::blocksEncryptedEntries()
{
    auto entries = regularFiles();
    entries[0].flags = 1;
    QTemporaryDir dir;
    QFile output(dir.filePath(QStringLiteral("encrypted.zip")));
    QVERIFY(output.open(QIODevice::WriteOnly));
    output.write(zip(entries)); output.close();
    QString error;
    QVERIFY(!Mattermost::auditWindowsUpdateZip(output.fileName(), &error));
}

void WindowsZipPolicyTest::blocksMismatchedDirectory()
{
    QByteArray bytes = zip(regularFiles());
    bytes.chop(2); // invalid EOCD length
    QTemporaryDir dir;
    QFile output(dir.filePath(QStringLiteral("truncated.zip")));
    QVERIFY(output.open(QIODevice::WriteOnly));
    output.write(bytes); output.close();
    QString error;
    QVERIFY(!Mattermost::auditWindowsUpdateZip(output.fileName(), &error));
}

QTEST_APPLESS_MAIN(WindowsZipPolicyTest)
#include "WindowsZipPolicyTest.moc"
