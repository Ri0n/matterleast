#include "PluginSignature.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#ifdef MATTERLEAST_TRUSTED_PLUGINS_ONLY
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "TrustedPluginKey.h"
#endif

namespace Mattermost {
namespace {

#ifdef MATTERLEAST_TRUSTED_PLUGINS_ONLY
bool verifyEd25519(const QByteArray& bytes, const QByteArray& signature)
{
    if (signature.size() != 64) {
        return false;
    }
    BIO* input = BIO_new_mem_buf(TrustedPluginPublicKeyPem, -1);
    if (!input) return false;
    EVP_PKEY* key = PEM_read_bio_PUBKEY(input, nullptr, nullptr, nullptr);
    BIO_free(input);
    if (!key) return false;
    const bool correctType = EVP_PKEY_base_id(key) == EVP_PKEY_ED25519;
    EVP_MD_CTX* ctx = correctType ? EVP_MD_CTX_new() : nullptr;
    const bool verified = ctx
        && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1
        && EVP_DigestVerify(
            ctx,
            reinterpret_cast<const unsigned char*>(signature.constData()),
            static_cast<size_t>(signature.size()),
            reinterpret_cast<const unsigned char*>(bytes.constData()),
            static_cast<size_t>(bytes.size())) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return verified;
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
    const QByteArray signature = signatureFile.read(65);
    if (signature.size() != 64) return {};

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
    if (!verifyEd25519(bytes, signature)) {
        QFile::remove(copyPath);
        return {};
    }
    return copyPath;
#endif
}

} // namespace Mattermost
