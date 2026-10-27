#include <QtTest>

#include <QFile>
#include "update/UpdateTrustPolicy.h"

using namespace Mattermost;

class UpdateTrustPolicyTest final : public QObject
{
    Q_OBJECT
private slots:
    void endpointPolicy()
    {
        if (!updateChecksCompiled()) {
            QVERIFY(updateManifestUrl(UpdateChannel::Stable).isEmpty());
            QVERIFY(updateManifestUrl(UpdateChannel::Nightly).isEmpty());
            QVERIFY(!updateManifestSignatureRequired());
            return;
        }
        const QString stable = updateManifestUrl(UpdateChannel::Stable);
        const QString nightly = updateManifestUrl(UpdateChannel::Nightly);
        QVERIFY(stable.startsWith(QStringLiteral("https://")));
        QVERIFY(nightly.startsWith(QStringLiteral("https://")));
        QVERIFY(!stable.contains(QStringLiteral("{channel}")));
        QVERIFY(!nightly.contains(QStringLiteral("{channel}")));
    }

    void rejectUnauthenticatedManifest()
    {
        const QByteArray manifest(R"({"schema":1,"channel":"stable"})");
        QVERIFY(!authenticateUpdateManifest(manifest, QByteArray()));
        QVERIFY(!authenticateUpdateManifest(QByteArray(), QByteArray(384, 'x')));
        QVERIFY(!authenticateUpdateManifest(manifest, QByteArray(384, 'x')));
    }

    void signedManifestAndTampering()
    {
        const QByteArray path = qgetenv("MATTERLEAST_SIGNED_UPDATE_TEST_PATH");
        if (!updateManifestSignatureRequired()) {
            QVERIFY(!authenticateUpdateManifest(QByteArray("untrusted"), QByteArray("bad")));
            return;
        }
        QVERIFY2(!path.isEmpty(), "A signed corporate build must provide a signed test manifest");
        QFile raw(QString::fromUtf8(path));
        QFile sidecar(QString::fromUtf8(path) + QStringLiteral(".sig"));
        QVERIFY(raw.open(QIODevice::ReadOnly));
        QVERIFY(sidecar.open(QIODevice::ReadOnly));
        const QByteArray manifest = raw.readAll();
        const QByteArray signature = sidecar.readAll();
        QVERIFY(!manifest.isEmpty());
        QVERIFY(authenticateUpdateManifest(manifest, signature));
        QByteArray modified = manifest;
        modified[0] = modified[0] == '{' ? '[' : '{';
        QVERIFY(!authenticateUpdateManifest(modified, signature));
        QByteArray changedSignature = signature;
        changedSignature[0] = static_cast<char>(changedSignature[0] ^ 1);
        QVERIFY(!authenticateUpdateManifest(manifest, changedSignature));
        QVERIFY(!authenticateUpdateManifest(manifest, signature.left(64)));
    }
};

QTEST_APPLESS_MAIN(UpdateTrustPolicyTest)
#include "UpdateTrustPolicyTest.moc"
