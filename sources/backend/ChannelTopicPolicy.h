#pragma once

#include <QString>

namespace Mattermost {

// Mattermost channel headers (topics) are Markdown and limited to 1024 UTF-16
// code units, matching the web client's input length semantics.
// Authorization is always enforced by the server's channel permission policy.
struct ChannelTopicPolicy
{
    static constexpr int MaximumLength = 1024;

    static bool valid(const QString& header)
    {
        return header.size() <= MaximumLength;
    }

    static bool changed(const QString& original, const QString& candidate)
    {
        return candidate != original;
    }
};

} // namespace Mattermost
