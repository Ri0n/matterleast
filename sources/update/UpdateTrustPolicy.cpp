#include "UpdateTrustPolicy.h"
#include "UpdateTrustConfig.h"

#include "crypto/RsaSha256Verifier.h"

namespace Mattermost {

QString defaultUpdateChannelName()
{
    return QString::fromLatin1(UpdateTrustConfig::defaultChannel);
}

bool updateChecksCompiled()
{
    return UpdateTrustConfig::checksCompiled;
}

bool updateManifestSignatureRequired()
{
    return UpdateTrustConfig::checksCompiled && UpdateTrustConfig::signaturesRequired;
}

QString updateManifestUrl(UpdateChannel channel)
{
    if (!UpdateTrustConfig::checksCompiled)
        return {};
    return QString::fromUtf8(channel == UpdateChannel::Stable
        ? UpdateTrustConfig::stableManifestUrl
        : UpdateTrustConfig::nightlyManifestUrl);
}

bool authenticateUpdateManifest(const QByteArray& manifest,
                                const QByteArray& detachedSignature)
{
#if MATTERLEAST_COMPILED_UPDATE_CHECKS
    if (!UpdateTrustConfig::signaturesRequired)
        return false;
    return verifyRsaSha256(manifest, detachedSignature,
                           QByteArray(UpdateTrustConfig::publicKeyPem));
#else
    Q_UNUSED(manifest)
    Q_UNUSED(detachedSignature)
    return false;
#endif
}

} // namespace Mattermost
