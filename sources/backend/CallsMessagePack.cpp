#include "CallsMessagePack.h"

#include <QJsonDocument>
#include <QtEndian>

namespace Mattermost::CallsMessagePack {
namespace {

void putByte(QByteArray& data, quint8 value)
{
    // Cast a runtime byte, not a >127 integer literal: MSVC /WX considers
    // char(0xcc) a truncated constant (C4310) even though it is valid wire data.
    data.append(static_cast<char>(value));
}

void putUnsigned(QByteArray& data, quint32 n)
{
    if (n <= 0x7f) {
        putByte(data, static_cast<quint8>(n));
    } else if (n <= 0xff) {
        putByte(data, static_cast<quint8>(0xcc));
        putByte(data, static_cast<quint8>(n));
    } else if (n <= 0xffff) {
        putByte(data, static_cast<quint8>(0xcd));
        const quint16 be = qToBigEndian(quint16(n));
        data.append(reinterpret_cast<const char*>(&be), sizeof(be));
    } else {
        putByte(data, static_cast<quint8>(0xce));
        const quint32 be = qToBigEndian(n);
        data.append(reinterpret_cast<const char*>(&be), sizeof(be));
    }
}

void putString(QByteArray& data, const QByteArray& value)
{
    const auto length = quint32(value.size());
    if (length <= 31) {
        putByte(data, static_cast<quint8>(0xa0 | length));
    } else if (length <= 0xff) {
        putByte(data, static_cast<quint8>(0xd9));
        putByte(data, static_cast<quint8>(length));
    } else if (length <= 0xffff) {
        putByte(data, static_cast<quint8>(0xda));
        const quint16 be = qToBigEndian(quint16(length));
        data.append(reinterpret_cast<const char*>(&be), sizeof(be));
    } else {
        putByte(data, static_cast<quint8>(0xdb));
        const quint32 be = qToBigEndian(length);
        data.append(reinterpret_cast<const char*>(&be), sizeof(be));
    }
    data.append(value);
}

void putBinary(QByteArray& data, const QByteArray& value)
{
    const auto length = quint32(value.size());
    if (length <= 0xff) {
        putByte(data, static_cast<quint8>(0xc4));
        putByte(data, static_cast<quint8>(length));
    } else if (length <= 0xffff) {
        putByte(data, static_cast<quint8>(0xc5));
        const quint16 be = qToBigEndian(quint16(length));
        data.append(reinterpret_cast<const char*>(&be), sizeof(be));
    } else {
        putByte(data, static_cast<quint8>(0xc6));
        const quint32 be = qToBigEndian(length);
        data.append(reinterpret_cast<const char*>(&be), sizeof(be));
    }
    data.append(value);
}

} // namespace

QByteArray encodeSdp(int sequence, const QJsonObject& description)
{
    if (sequence <= 0 || description.isEmpty()
        || !description.value(QStringLiteral("sdp")).isString()
        || (description.value(QStringLiteral("type")).toString() != QLatin1String("offer")
            && description.value(QStringLiteral("type")).toString()
               != QLatin1String("answer"))) {
        return {};
    }

    const QByteArray json = QJsonDocument(description).toJson(QJsonDocument::Compact);
    // qCompress adds a 4-byte uncompressed-length header before the standard
    // zlib stream; Calls expects only the zlib bytes produced by fflate.
    const QByteArray compressed = qCompress(json, 6).mid(4);

    QByteArray frame;
    frame.reserve(compressed.size() + 100);
    putByte(frame, static_cast<quint8>(0x83)); // MessagePack map(3).
    putString(frame, QByteArrayLiteral("action"));
    putString(frame, QByteArrayLiteral("custom_com.mattermost.calls_sdp"));
    putString(frame, QByteArrayLiteral("seq"));
    putUnsigned(frame, quint32(sequence));
    putString(frame, QByteArrayLiteral("data"));
    putByte(frame, static_cast<quint8>(0x81)); // MessagePack map(1).
    putString(frame, QByteArrayLiteral("data"));
    putBinary(frame, compressed);
    return frame;
}

} // namespace Mattermost::CallsMessagePack
