#include "PluginSignature.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#ifdef MATTERLEAST_TRUSTED_PLUGINS_ONLY
#include <QtCrypto>
#include "TrustedPluginKey.h"
#endif

namespace Mattermost {
namespace {

#ifdef MATTERLEAST_TRUSTED_PLUGINS_ONLY
// The verification policy is independent of the installed crypto backend:
// QCA2 and QCA3 both expose RSA PKCS#1 v1.5 with SHA-256 as EMSA3_SHA256.
// A failed key import, missing provider or invalid signature must deny loading.
bool verifyRsaSha256(const QByteArray& bytes, const QByteArray& signature)
{
    static QCA::Initializer _qca;
    Q_UNUSED(_qca)

    QCA::ConvertResult result = QCA::ConvertGood;
    const QCA::PublicKey key = QCA::PublicKey::fromPEM(
        QString::fromUtf8(TrustedPluginPublicKeyPem), &result);
    if (result != QCA::ConvertGood || key.isNull() || !key.isRSA()
        || !key.canVerify() || key.bitSize() < 2048 || key.bitSize() > 8192
        || signature.size() != (key.bitSize() + 7) / 8) {
        return false;
    }
    return key.verifyMessage(bytes, signature, QCA::EMSA3_SHA256);
}
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
    if (!verifyRsaSha256(bytes, signature)) {
        QFile::remove(copyPath);
        return {};
    }
    return copyPath;
#endif
}

} // namespace Mattermost
