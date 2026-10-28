// Test-only trust policy. Uses the *production* QCA RSA/SHA-256 verifier,
// with a disposable public key; no verification bypass is compiled here.
#include "update/UpdateTrustPolicy.h"
#include "crypto/RsaSha256Verifier.h"

#include <QFile>
#include <QtPlugin>

Q_IMPORT_PLUGIN(qca_wincrypto)

namespace Mattermost {
bool updateChecksCompiled() { return true; }
QString defaultUpdateChannelName() { return QStringLiteral("nightly"); }
bool updateManifestSignatureRequired() { return true; }
QString updateManifestUrl(UpdateChannel)
{
    return qEnvironmentVariable("MATTERLEAST_SIGNED_SMOKE_URL");
}
bool authenticateUpdateManifest(const QByteArray& raw, const QByteArray& sig)
{
    QFile publicKey(qEnvironmentVariable("MATTERLEAST_SIGNED_SMOKE_PUBLIC_KEY"));
    if (!publicKey.open(QIODevice::ReadOnly))
        return false;
    return verifyRsaSha256(raw, sig, publicKey.readAll());
}
} // namespace Mattermost
