// SPDX-License-Identifier: LGPL-3.0-or-later
#pragma once

#include <QList>
#include <QString>

// Pure Qt presentation data. GStreamer device objects never cross the
// media worker boundary or enter the call window.
enum class CallsDeviceKind { Microphone, Camera };

struct CallsMediaDevice {
    QString id;
    QString displayName;
    CallsDeviceKind kind = CallsDeviceKind::Microphone;
};
