// SPDX-License-Identifier: LGPL-3.0-or-later
#include <QtTest>

#include "plugins/calls-gstreamer/CallsMediaMap.h"

class CallsMediaMapTest final : public QObject
{
    Q_OBJECT
private slots:
    void decodesVideoMap()
    {
        // MessageTypeMediaMap=9, map with one VP8 video sender and one
        // audio sender. The Go MessagePack encoder can use the uint8 marker.
        const QByteArray fixture = QByteArray::fromHex(
            "cc0982"
            "a13182a474797065a5766964656fa973656e6465725f6964a973657373696f6e2d31"
            "a13282a474797065a5766f696365a973656e6465725f6964a973657373696f6e2d32");
        const auto mapping = decodeRtcdVideoMediaMap(fixture);
        QCOMPARE(mapping.size(), 1);
        QCOMPARE(mapping.value(QStringLiteral("1")), QStringLiteral("session-1"));
        QVERIFY(mapping.value(QStringLiteral("2")).isEmpty());
        QByteArray fixInt = fixture.mid(1);
        QVERIFY(fixInt.startsWith(char(9)));
        QCOMPARE(decodeRtcdVideoMediaMap(fixInt), mapping);
    }

    void rejectsMalformedFrames()
    {
        QVERIFY(decodeRtcdVideoMediaMap({}).isEmpty());
        QVERIFY(decodeRtcdVideoMediaMap(QByteArray::fromHex("cc09")).isEmpty());
        QVERIFY(decodeRtcdVideoMediaMap(QByteArray::fromHex("cc0981a131")).isEmpty());
        QVERIFY(decodeRtcdVideoMediaMap(QByteArray(65537, 'a')).isEmpty());
    }
};

QTEST_APPLESS_MAIN(CallsMediaMapTest)
#include "CallsMediaMapTest.moc"
