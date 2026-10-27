#pragma once

#include <QByteArray>
#include <QString>

namespace Mattermost {
enum class UpdateChannel { Stable, Nightly };

// These values are build-time fixed: corporate URL/key cannot be overridden
// by network content or a local untrusted preference.
bool updateChecksCompiled();
bool updateManifestSignatureRequired();
QString updateManifestUrl(UpdateChannel channel);

// Verify the detached <manifest>.sig signature over the raw manifest bytes.
// Returns false if checks are compiled out, no trust key is configured,
// the signature is malformed, or verification fails.
bool authenticateUpdateManifest(const QByteArray& manifest,
                                const QByteArray& detachedSignature);
} // namespace Mattermost
