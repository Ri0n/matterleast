#pragma once

#include <QString>

namespace Mattermost {

/**
 * Re-activating the same already-open conversation row is presentation-only.
 *
 * The Following/Attention resume cursor is mutable because viewport reads can
 * advance FirstUnread while the row remains selected. A repeated click on that
 * same conversation must therefore preserve the current viewport instead of
 * turning the row into a "next unread" command. Threads deliberately retain
 * their ordered unread-reply resume semantics.
 */
inline bool shouldPreserveRepeatedConversationActivation(
    bool isThread,
    const QString& entryChannelId,
    const QString& currentChannelId,
    bool repeatedActivation)
{
    return repeatedActivation
        && !isThread
        && !entryChannelId.isEmpty()
        && entryChannelId == currentChannelId;
}

} // namespace Mattermost
