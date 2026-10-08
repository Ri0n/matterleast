#pragma once

namespace Mattermost {

// Additional eligibility for ordinary channel root posts. Existing mention,
// DM/GM, muted-channel and followed-thread notification rules remain unchanged.
inline bool shouldNotifyRootPost(bool directConversation,
                                 bool mentioned,
                                 bool favorite)
{
    return directConversation || mentioned || favorite;
}

} // namespace Mattermost
