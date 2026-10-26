#include "LocalMessageCrypto.h"

#include <QtCrypto>

namespace Mattermost {
namespace {

// Use separate nonces for each record. QCA authenticates ciphertext and its tag;
// the encrypted prefix binds the envelope to its intended storage context.
const QByteArray Magic("MLAE1", 5);
constexpr int NonceBytes = 12;
constexpr int TagBytes = 16;

QCA::Initializer& cryptoInitializer()
{
    static QCA::Initializer initializer;
    return initializer;
}

bool canEncrypt()
{
    (void)cryptoInitializer();
    return QCA::isSupported(QStringLiteral("aes256-gcm"));
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

    QCA::Cipher cipher(QStringLiteral("aes256"), QCA::Cipher::GCM,
                       QCA::Cipher::NoPadding, QCA::Encode,
                       QCA::SymmetricKey(key), QCA::InitializationVector(nonce));
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
