#pragma once
#include <QString>
#include <QtGlobal>

namespace Mattermost::VoiceRecordingPolicy {

// Use a decimal 100 MB limit, not 100 MiB (104,857,600 bytes).
// This is also below Mattermost's typical 100 MiB server attachment limit;
// admins may set a lower cap, which the uploader still enforces.
constexpr qint64 MaxVoiceBytes = 100LL * 1000 * 1000;
constexpr qint64 MinimumVoiceMs = 350;

// Some encoders (especially long AAC/M4A recordings) write their sample
// index and other container metadata only when stop() finalizes the file.
// A 256 KiB reserve can be too small for several hours of audio. Stop at
// 95 MB to leave 5 MB for that metadata and buffered encoder output.
// This is a size-based safety margin, not an arbitrary recording time limit.
constexpr qint64 ContainerFinalizationReserve = 5LL * 1000 * 1000;

inline bool shouldStopForSize(qint64 bytes) {
    return bytes >= MaxVoiceBytes - ContainerFinalizationReserve;
}

inline bool canOfferMicrophone(bool hasText, bool hasAttachments,
                               bool editing, bool sending) {
    return !hasText && !hasAttachments && !editing && !sending;
}

// Check the finalized file, never just the size observed while recording.
// Require strictly less than 100 MB so there is no boundary ambiguity.
inline bool withinLimit(qint64 bytes) {
    return bytes > 0 && bytes < MaxVoiceBytes;
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
