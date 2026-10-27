#pragma once

#include <QString>

class QTemporaryDir;

namespace Mattermost {

/**
 * Returns a verified immutable snapshot path when trusted-plugin mode is
 * enabled, or the original path in ordinary community builds.
 *
 * The detached signature contains an RSA PKCS#1 v1.5 SHA-256 signature in <library>.sig and covers
 * the entire binary. Verification always precedes plugin metadata and load.
 */
QString trustedPluginPath(const QString& filePath, QTemporaryDir* staging);

} // namespace Mattermost
