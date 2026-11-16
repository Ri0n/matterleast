#pragma once
#include <QString>
#include <QtGlobal>

namespace Mattermost::VoiceRecordingPolicy {

// Mattermost's default maximum attachment size is 100 MiB. Admins may
// configure a lower limit; the upload service still handles server rejection.
constexpr qint64 MaxVoiceBytes = 100LL * 1024 * 1024;
constexpr qint64 MinimumVoiceMs = 350;
constexpr qint64 ContainerFinalizationReserve = 256LL * 1024;

// Close the encoder slightly before the hard cap to make room for the
// muxer's final index/headers written by stop(). No clock-based cutoff.
inline bool shouldStopForSize(qint64 bytes) {
    return bytes >= MaxVoiceBytes - ContainerFinalizationReserve;
}

inline bool canOfferMicrophone(bool hasText, bool hasAttachments,
                               bool editing, bool sending) {
    return !hasText && !hasAttachments && !editing && !sending;
}

// This is a size bound, never an arbitrary wall-clock recording limit.
inline bool withinLimit(qint64 bytes) {
    return bytes > 0 && bytes <= MaxVoiceBytes;
}

inline QString elapsedLabel(qint64 elapsedMs) {
    const qint64 total = qMax<qint64>(0, elapsedMs) / 1000;
    const qint64 hours = total / 3600;
    const qint64 minutes = (total / 60) % 60;
    const qint64 seconds = total % 60;
    const QString clock = QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
    return hours == 0 ? clock : QString::number(hours) + QLatin1Char(':') + clock;
}
} // namespace Mattermost::VoiceRecordingPolicy
