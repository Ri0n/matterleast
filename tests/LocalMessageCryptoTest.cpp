#include <QtTest>

#include "backend/LocalMessageCrypto.h"

using namespace Mattermost;

class LocalMessageCryptoTest final : public QObject
{
    Q_OBJECT
private slots:
    void roundTripTamperAndContextBinding()
    {
        QVERIFY2(LocalMessageCrypto::isAvailable(), "QCA AES-256-GCM provider is required");
        const QByteArray key = LocalMessageCrypto::randomKey();
        QCOMPARE(key.size(), LocalMessageCrypto::KeyBytes);

        const QByteArray message = QByteArray::fromHex(
            "d09fd180d0b8d0b2d0b5d1822051756f746564206d657373616765");
        const QByteArray context("post-cache-v1");
        const QByteArray ciphertext = LocalMessageCrypto::encrypt(message, key, context);
        QVERIFY(!ciphertext.isEmpty());
        QVERIFY(!ciphertext.contains(message));
        QVERIFY(ciphertext != LocalMessageCrypto::encrypt(message, key, context));

        QByteArray decoded;
        QVERIFY(LocalMessageCrypto::decrypt(ciphertext, key, context, &decoded));
        QCOMPARE(decoded, message);
        QVERIFY(!LocalMessageCrypto::decrypt(ciphertext, key, "other-record", &decoded));
        QVERIFY(!LocalMessageCrypto::decrypt(ciphertext, QByteArray(32, 'x'), context, &decoded));

        QByteArray altered = ciphertext;
        altered[altered.size() - 1] = char(altered.at(altered.size() - 1) ^ 0x01);
        QVERIFY(!LocalMessageCrypto::decrypt(altered, key, context, &decoded));
        QVERIFY(decoded.isEmpty());

        altered = ciphertext;
        altered[6] = char(altered.at(6) ^ 0x01);
        QVERIFY(!LocalMessageCrypto::decrypt(altered, key, context, &decoded));
    }

    void noKeyNoPersistence()
    {
        QVERIFY(LocalMessageCrypto::encrypt("secret", {}).isEmpty());
        QByteArray output;
        QVERIFY(!LocalMessageCrypto::decrypt("plain text", QByteArray(32, 'k'),
                                             {}, &output));
        QVERIFY(output.isEmpty());
    }
};

QTEST_APPLESS_MAIN(LocalMessageCryptoTest)
#include "LocalMessageCryptoTest.moc"
