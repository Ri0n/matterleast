#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "plugins/PluginSignature.h"

using namespace Mattermost;

class TrustedPluginLoadTest final : public QObject
{
    Q_OBJECT
private slots:
    void acceptsSignedLibraryAndRejectsAlterations()
    {
        const QString source = QString::fromUtf8(
            qgetenv("MATTERLEAST_SIGNED_PLUGIN_TEST_PATH"));
        QVERIFY2(!source.isEmpty(), "signed plugin target is not configured");

        QTemporaryDir verified;
        QVERIFY(verified.isValid());
        const QString staged = trustedPluginPath(source, &verified);
        QVERIFY2(!staged.isEmpty(), "genuine signed plugin was rejected");
        QVERIFY(QFile::exists(staged));
        QVERIFY(staged != source);

        QTemporaryDir scratch;
        QVERIFY(scratch.isValid());
        const QString changed = scratch.filePath(QStringLiteral("altered-plugin.bin"));
        QVERIFY(QFile::copy(source, changed));
        QVERIFY(QFile::copy(source + QStringLiteral(".sig"),
                            changed + QStringLiteral(".sig")));
        {
            QFile file(changed);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.seek(0));
            const char altered = '\\x7e';
            QCOMPARE(file.write(&altered, 1), qint64(1));
        }
        QVERIFY(trustedPluginPath(changed, &verified).isEmpty());

        const QString unsignedPath = scratch.filePath(QStringLiteral("unsigned.bin"));
        QVERIFY(QFile::copy(source, unsignedPath));
        QVERIFY(trustedPluginPath(unsignedPath, &verified).isEmpty());

        // Invalid signature length cannot bypass verification.
        const QString badLength = scratch.filePath(QStringLiteral("invalid.bin"));
        QVERIFY(QFile::copy(source, badLength));
        QFile shortSignature(badLength + QStringLiteral(".sig"));
        QVERIFY(shortSignature.open(QIODevice::WriteOnly));
        QCOMPARE(shortSignature.write("invalid", 7), qint64(7));
        shortSignature.close();
        QVERIFY(trustedPluginPath(badLength, &verified).isEmpty());
    }
};

QTEST_APPLESS_MAIN(TrustedPluginLoadTest)
#include "TrustedPluginLoadTest.moc"
