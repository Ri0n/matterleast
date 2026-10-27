#include "PluginSignature.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#ifdef MATTERLEAST_TRUSTED_PLUGINS_ONLY
#include "crypto/RsaSha256Verifier.h"
#include "TrustedPluginKey.h"
#endif

namespace Mattermost {
namespace {

#ifdef MATTERLEAST_TRUSTED_PLUGINS_ONLY

#endif

} // namespace

QString trustedPluginPath(const QString& filePath, QTemporaryDir* staging)
{
#ifndef MATTERLEAST_TRUSTED_PLUGINS_ONLY
    Q_UNUSED(staging)
    return filePath;
#else
    if (!staging || !staging->isValid()) return {};

    // Never execute from the original, potentially writable plugin folder.
    // Verify the very bytes that QPluginLoader will open from our private
    // per-process directory. Restrict the copy to a bounded binary size.
    const QFileInfo info(filePath);
    constexpr qint64 MaxPluginBytes = 128LL * 1024 * 1024;
    if (!info.isFile() || info.isSymLink() || info.size() <= 0
        || info.size() > MaxPluginBytes) {
        return {};
    }
    QFile signatureFile(filePath + QStringLiteral(".sig"));
    if (!signatureFile.open(QIODevice::ReadOnly)) return {};
    // RSA signatures are key-size dependent (256..1024 bytes for our
    // accepted 2048..8192-bit keys). Detect oversized sidecars as well.
    const QByteArray signature = signatureFile.read(1025);
    if (signature.size() < 256 || signature.size() > 1024) return {};

    const QByteArray nameHash = QCryptographicHash::hash(
        info.absoluteFilePath().toUtf8(), QCryptographicHash::Sha256).toHex();
    const QString copyPath = QDir(staging->path()).filePath(
        QString::fromLatin1(nameHash.left(24)) + QLatin1Char('-')
        + info.fileName());
    if (!QFile::copy(filePath, copyPath)) return {};
    QFile copy(copyPath);
    copy.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (!copy.open(QIODevice::ReadOnly)) {
        QFile::remove(copyPath);
        return {};
    }
    const QByteArray bytes = copy.readAll();
    copy.close();
    if (!verifyRsaSha256(bytes, signature, QByteArray(TrustedPluginPublicKeyPem))) {
        QFile::remove(copyPath);
        return {};
    }
    return copyPath;
#endif
}

} // namespace Mattermost
