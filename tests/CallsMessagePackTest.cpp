#include <QtTest/QtTest>

#include <QJsonDocument>
#include <QtEndian>

#include "backend/CallsMessagePack.h"

class CallsMessagePackTest final : public QObject
{
    Q_OBJECT
private slots:
    void encodesRealCallsBinarySdp()
    {
        const QJsonObject offer {
            {QStringLiteral("type"), QStringLiteral("offer")},
            {QStringLiteral("sdp"), QStringLiteral("v=0\\r\\nm=audio 9 UDP/TLS/RTP/SAVPF 111\\r\\n")}
        };
        const QByteArray frame = Mattermost::CallsMessagePack::encodeSdp(300, offer);
        QVERIFY(!frame.isEmpty());

        // Validate the exact MessagePack envelope and standard RFC1950 zlib
        // payload, rather than merely roundtripping the encoder itself.
        int cursor = 0;
        auto byte = [&] { return quint8(frame.at(cursor++)); };
        auto string = [&] {
            quint32 length = 0;
            const quint8 type = byte();
            if ((type & 0xe0) == 0xa0) {
                length = type & 0x1f;
            } else if (type == 0xd9) {
                length = byte();
            } else {
                return QByteArray();
            }
            const QByteArray result = frame.mid(cursor, int(length));
            cursor += int(length);
            return result;
        };
        QCOMPARE(byte(), quint8(0x83));
        QCOMPARE(string(), QByteArray("action"));
        QCOMPARE(string(), QByteArray("custom_com.mattermost.calls_sdp"));
        QCOMPARE(string(), QByteArray("seq"));
        QCOMPARE(byte(), quint8(0xcd));
        QCOMPARE(byte(), quint8(0x01));
        QCOMPARE(byte(), quint8(0x2c));
        QCOMPARE(string(), QByteArray("data"));
        QCOMPARE(byte(), quint8(0x81));
        QCOMPARE(string(), QByteArray("data"));
        const quint8 blobType = byte();
        quint32 size = 0;
        if (blobType == 0xc4) {
            size = byte();
        } else if (blobType == 0xc5) {
            size = quint32(byte()) << 8;
            size |= byte();
        } else {
            QFAIL("unexpected MessagePack binary encoding");
        }
        const QByteArray deflated = frame.mid(cursor, int(size));
        QCOMPARE(cursor + int(size), frame.size());
        // qUncompress consumes Qt's 4-byte original-length header.
        QByteArray withHeader;
        const quint32 be = qToBigEndian(quint32(
            QJsonDocument(offer).toJson(QJsonDocument::Compact).size()));
        withHeader.append(reinterpret_cast<const char*>(&be), sizeof(be));
        withHeader.append(deflated);
        QCOMPARE(QJsonDocument::fromJson(qUncompress(withHeader)).object(), offer);
    }

    void rejectsInvalidDescriptions()
    {
        QVERIFY(Mattermost::CallsMessagePack::encodeSdp(0, {}).isEmpty());
        QVERIFY(Mattermost::CallsMessagePack::encodeSdp(1, {
            {QStringLiteral("type"), QStringLiteral("invalid")},
            {QStringLiteral("sdp"), QStringLiteral("v=0")}
        }).isEmpty());
    }
};

QTEST_APPLESS_MAIN(CallsMessagePackTest)
#include "CallsMessagePackTest.moc"
