#pragma once

#include <QString>

namespace Mattermost {

// Mattermost accepts a custom status with an emoji, text, or both.
// An empty pair is handled by the UI as an explicit request to clear it.
inline bool hasCustomStatusContent(const QString& emoji, const QString& text)
{
    return !emoji.trimmed().isEmpty() || !text.trimmed().isEmpty();
}

inline bool isEmojiOnlyCustomStatus(const QString& emoji, const QString& text)
{
    return !emoji.trimmed().isEmpty() && text.trimmed().isEmpty();
}

} // namespace Mattermost
