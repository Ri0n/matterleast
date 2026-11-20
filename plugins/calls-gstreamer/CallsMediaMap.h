// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

// RTCD sends MessageTypeMediaMap (9) over calls-dc as a MessagePack
// scalar followed by a map: mid -> {type, sender_id}. Only video
// sender IDs are exposed to the Qt call window.
QHash<QString, QString> decodeRtcdVideoMediaMap(const QByteArray& payload);
