// SPDX-License-Identifier: LGPL-3.0-or-later
#include "CallsMediaMap.h"

#include <QtEndian>

namespace {

class Reader
{
public:
    explicit Reader(const QByteArray& data) : data_(data) {}

    int byte()
    {
        if (pos_ >= data_.size()) return -1;
        return static_cast<quint8>(data_.at(pos_++));
    }

    quint32 readN(int n, bool* ok)
    {
        if (n < 1 || n > 4 || pos_ + n > data_.size()) {
            *ok = false;
            return 0;
        }
        quint32 value = 0;
        for (int i = 0; i < n; ++i) {
            value = (value << 8) | static_cast<quint8>(data_.at(pos_++));
        }
        return value;
    }

    int count(bool* ok)
    {
        const int marker = byte();
        if (marker >= 0x80 && marker <= 0x8f) return marker & 0x0f;
        if (marker == 0xde) return int(readN(2, ok));
        if (marker == 0xdf) {
            const quint32 len = readN(4, ok);
            if (len > 256) *ok = false;
            return int(len);
        }
        *ok = false;
        return 0;
    }

    QString string(bool* ok)
    {
        const int marker = byte();
        int size = -1;
        if (marker >= 0xa0 && marker <= 0xbf) size = marker & 0x1f;
        else if (marker == 0xd9) size = int(readN(1, ok));
        else if (marker == 0xda) size = int(readN(2, ok));
        else if (marker == 0xdb) {
            const quint32 len = readN(4, ok);
            if (len > 512) *ok = false;
            size = int(len);
        }
        if (size < 0 || size > 512 || pos_ + size > data_.size()) {
            *ok = false;
            return {};
        }
        const QString value = QString::fromUtf8(data_.constData() + pos_, size);
        pos_ += size;
        return value;
    }

private:
    const QByteArray& data_;
    int pos_ = 0;
};

} // namespace

QHash<QString, QString> decodeRtcdVideoMediaMap(const QByteArray& payload)
{
    if (payload.size() < 2 || payload.size() > 65536) return {};
    Reader reader(payload);
    const int marker = reader.byte();
    // Go's msgpack encoder can preserve uint8 width (0xcc 0x09) rather
    // than writing the positive fixint 0x09.
    if (marker != 9 && (marker != 0xcc || reader.byte() != 9)) return {};
    bool ok = true;
    const int entries = reader.count(&ok);
    if (!ok || entries < 0 || entries > 256) return {};
    QHash<QString, QString> senders;
    for (int i = 0; i < entries; ++i) {
        const QString mid = reader.string(&ok);
        const int count = reader.count(&ok);
        if (!ok || count < 0 || count > 32) return {};
        QString type;
        QString sender;
        for (int j = 0; j < count; ++j) {
            const QString key = reader.string(&ok);
            const QString value = reader.string(&ok);
            if (!ok) return {};
            if (key == QLatin1String("type")) type = value;
            if (key == QLatin1String("sender_id")) sender = value;
        }
        if (!mid.isEmpty() && !sender.isEmpty() && type == QLatin1String("video")) {
            senders.insert(mid, sender);
        }
    }
    return senders;
}
