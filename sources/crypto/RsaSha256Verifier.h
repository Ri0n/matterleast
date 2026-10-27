#pragma once

#include <QByteArray>

namespace Mattermost {
// Detached signature is RSA PKCS#1 v1.5 with SHA-256, over the exact
// byte sequence received. No text normalization or JSON reserialization.
bool verifyRsaSha256(const QByteArray& data, const QByteArray& signature,
                     const QByteArray& publicKeyPem);
}
