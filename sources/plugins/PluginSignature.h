#pragma once

#include <QString>

class QTemporaryDir;

namespace Mattermost {

/**
 * Returns a verified immutable snapshot path when trusted-plugin mode is
 * enabled, or the original path in ordinary community builds.
 *
 * The detached signature is 64 raw Ed25519 bytes in <library>.sig and covers
 * the entire binary. Verification always precedes plugin metadata and load.
 */
QString trustedPluginPath(const QString& filePath, QTemporaryDir* staging);

} // namespace Mattermost
