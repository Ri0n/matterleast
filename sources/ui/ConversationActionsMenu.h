#pragma once

#include <QString>

class QMenu;
class QWidget;

namespace Mattermost {

class Backend;
class BackendChannel;

// One source of actions, labels, icons, permissions and handlers for the
// conversation header and the Channels/Unread/Attention context menus.
// QMenu remains owned by the caller: persistent in ChatArea, ephemeral for
// sidebar right-clicks. All actions resolve the channel by ID on activation.
class ConversationActionsMenu
{
public:
    static void populate(QMenu& menu, Backend& backend,
                         const QString& channelId, const QString& teamId,
                         QWidget* dialogParent, bool thread = false);

    static QString channelWebUrl(Backend& backend, const BackendChannel& channel,
                                 const QString& preferredTeamId = {});
};

} // namespace Mattermost
