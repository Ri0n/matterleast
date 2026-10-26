#pragma once

#include <QByteArray>

namespace Mattermost {

// Authenticated, versioned AES-256-GCM envelope for local message contents.
// Never write plaintext when no valid encryption key/provider is available.
class LocalMessageCrypto
{
public:
    static constexpr int KeyBytes = 32;

    static bool isAvailable();
    static QByteArray randomKey();
    static QByteArray encrypt(const QByteArray& cleartext,
                              const QByteArray& key,
                              const QByteArray& context = {});
    static bool decrypt(const QByteArray& envelope,
                        const QByteArray& key,
                        const QByteArray& context,
                        QByteArray* cleartext);
};

} // namespace Mattermost
