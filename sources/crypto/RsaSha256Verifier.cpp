#include "RsaSha256Verifier.h"

#include <QtCrypto>

namespace Mattermost {
bool verifyRsaSha256(const QByteArray& data, const QByteArray& signature,
                     const QByteArray& publicKeyPem)
{
    if (data.isEmpty() || signature.isEmpty() || publicKeyPem.isEmpty())
        return false;

    static QCA::Initializer _qca;
    Q_UNUSED(_qca)
    QCA::ConvertResult result = QCA::ConvertGood;
    QCA::PublicKey key = QCA::PublicKey::fromPEM(QString::fromUtf8(publicKeyPem), &result);
    if (result != QCA::ConvertGood || key.isNull() || !key.isRSA()
        || !key.canVerify() || key.bitSize() < 2048 || key.bitSize() > 8192
        || signature.size() != (key.bitSize() + 7) / 8)
        return false;

    return key.verifyMessage(data, signature, QCA::EMSA3_SHA256);
}
} // namespace Mattermost
