#pragma once

#include <QString>
#include <Qt>

namespace Mattermost {

enum class TaskbarAttention {
    None,
    NativeAlert,
    LauncherUrgency,
};

inline TaskbarAttention taskbarAttentionFor(const QString& platform, Qt::ApplicationState state)
{
    if (state == Qt::ApplicationActive) {
        return TaskbarAttention::None;
    }

    // These Qt backends have a dedicated attention mechanism. Qt Wayland
    // implements alert() as an xdg-activation request instead.
    if (platform == QLatin1String("xcb")
        || platform == QLatin1String("windows")
        || platform == QLatin1String("cocoa")) {
        return TaskbarAttention::NativeAlert;
    }
    if (platform.startsWith(QLatin1String("wayland"))) {
        return TaskbarAttention::LauncherUrgency;
    }
    return TaskbarAttention::None;
}

} // namespace Mattermost
