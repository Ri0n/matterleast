#include "LocalMessageCrypto.h"

#include <QtCrypto>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>

#if defined(MATTERLEAST_STATIC_QCA_OSSL) || defined(MATTERLEAST_STATIC_QCA_WINCRYPTO)
#include <QtPlugin>
// Every standalone crypto test and the application import the static provider.
#if defined(MATTERLEAST_STATIC_QCA_WINCRYPTO)
Q_IMPORT_PLUGIN(qca_wincrypto)
#else
Q_IMPORT_PLUGIN(qca_ossl)
#endif
#endif

namespace Mattermost {
namespace {

// Use separate nonces for each record. QCA authenticates ciphertext and its tag;
// the encrypted prefix binds the envelope to its intended storage context.
const QByteArray Magic("MLAE1", 5);
constexpr int NonceBytes = 12;
constexpr int TagBytes = 16;

QCA::Initializer& cryptoInitializer()
{
    // Bundled QCA providers are installed beside the application. QCA does not
    // automatically discover its private plugin directory when the package is
    // relocated (especially in AppImage), so add it before QCA initializes.
    // A developer-provided QCA_PLUGIN_PATH is respected.
    static QCA::Initializer* initializer = [] {
#if !defined(MATTERLEAST_STATIC_QCA_OSSL) && !defined(MATTERLEAST_STATIC_QCA_WINCRYPTO)
        if (qEnvironmentVariableIsEmpty("QCA_PLUGIN_PATH")
            && QCoreApplication::instance()) {
            const QDir appDir(QCoreApplication::applicationDirPath());
            const QString suffix = QStringLiteral("qca3-qt%1").arg(QT_VERSION_MAJOR);
            const QStringList candidatePaths {
                appDir.filePath(QStringLiteral("lib/") + suffix),
                appDir.filePath(QStringLiteral("../lib/") + suffix),
            };
            for (const QString& candidate : candidatePaths) {
                const QFileInfo pluginDir(QDir(candidate).filePath(QStringLiteral("crypto")));
                if (pluginDir.isDir()) {
                    qputenv("QCA_PLUGIN_PATH", QFile::encodeName(candidate));
                    break;
                }
            }
        }
#endif
        return new QCA::Initializer();
    }();
    return *initializer;
}

bool canEncrypt()
{
    (void)cryptoInitializer();
    return QCA::isSupported("aes256-gcm");
}

} // namespace

bool LocalMessageCrypto::isAvailable()
{
    return canEncrypt();
}

QByteArray LocalMessageCrypto::randomKey()
{
    if (!canEncrypt()) return {};
    return QCA::Random::randomArray(KeyBytes).toByteArray();
}

QByteArray LocalMessageCrypto::encrypt(const QByteArray& cleartext,
                                       const QByteArray& key,
                                       const QByteArray& context)
{
    if (key.size() != KeyBytes || !canEncrypt()) return {};

    const QByteArray nonce = QCA::Random::randomArray(NonceBytes).toByteArray();
    if (nonce.size() != NonceBytes) return {};

    // QCA requires an explicit-sized AuthTag for AES-GCM encryption.
    // Without it OpenSSL does not extract a tag at final(), so unauthenticated
    // ciphertext would otherwise be produced.
    QCA::Cipher cipher(QStringLiteral("aes256"), QCA::Cipher::GCM,
                       QCA::Cipher::NoPadding, QCA::Encode,
                       QCA::SymmetricKey(key), QCA::InitializationVector(nonce),
                       QCA::AuthTag(TagBytes));
    QByteArray input = context;
    input.append(char(0));
    input.append(cleartext);

    QByteArray encrypted = cipher.update(input).toByteArray();
    encrypted += cipher.final().toByteArray();
    if (!cipher.ok()) return {};
    const QByteArray tag = cipher.tag().toByteArray();
    if (tag.size() != TagBytes) return {};

    return Magic + nonce + tag + encrypted;
}

bool LocalMessageCrypto::decrypt(const QByteArray& envelope,
                                 const QByteArray& key,
                                 const QByteArray& context,
                                 QByteArray* cleartext)
{
    if (cleartext) cleartext->clear();
    if (!cleartext || key.size() != KeyBytes || !canEncrypt()
        || envelope.size() < Magic.size() + NonceBytes + TagBytes + 1
        || !envelope.startsWith(Magic)) {
        return false;
    }
    const int prefix = Magic.size();
    const QByteArray nonce = envelope.mid(prefix, NonceBytes);
    const QByteArray tag = envelope.mid(prefix + NonceBytes, TagBytes);
    const QByteArray encrypted = envelope.mid(prefix + NonceBytes + TagBytes);
    QCA::Cipher cipher(QStringLiteral("aes256"), QCA::Cipher::GCM,
                       QCA::Cipher::NoPadding, QCA::Decode,
                       QCA::SymmetricKey(key), QCA::InitializationVector(nonce),
                       QCA::AuthTag(tag));
    QByteArray output = cipher.update(encrypted).toByteArray();
    output += cipher.final().toByteArray();
    if (!cipher.ok()) return false;

    const QByteArray expected = context + QByteArray(1, char(0));
    if (!output.startsWith(expected)) return false;
    *cleartext = output.mid(expected.size());
    return true;
}

} // namespace Mattermost
