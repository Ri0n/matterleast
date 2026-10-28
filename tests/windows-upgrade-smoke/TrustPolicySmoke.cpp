// Public unsigned-community update source only. Do not ship this stub.
// The production UpdateTrustPolicyTest checks corporate RSA signatures.
#include "update/UpdateTrustPolicy.h"

namespace Mattermost {
bool updateChecksCompiled() { return true; }
bool updateManifestSignatureRequired() { return false; }
QString updateManifestUrl(UpdateChannel channel)
{
    const QString name = channel == UpdateChannel::Nightly
        ? QStringLiteral("nightly") : QStringLiteral("stable");
    return QStringLiteral("https://github.com/Ri0n/matterleast/releases/download/updates-%1/windows-x64.json")
        .arg(name);
}
bool authenticateUpdateManifest(const QByteArray&, const QByteArray&) { return false; }
} // namespace Mattermost
