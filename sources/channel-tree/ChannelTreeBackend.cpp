#include "ChannelTree.h"

#include "backend/Backend.h"

#include "mainwindow.h"

namespace Mattermost {

Backend* ChannelTree::backendInstance() const
{
    if (_visibleAccountRootIds.size() > 1)
        return nullptr;
    if (_visibleAccountRootIds.size() == 1)
        return backendForAccount(_visibleAccountRootIds.front());
    // This legacy API has no account parameter. Return a Backend only when
    // there is a unique live owner; never choose the primary server from a
    // multi-account presentation.
    Backend* unique = backendForSidebar.data();
    for (auto it = _accountBackends.cbegin(); it != _accountBackends.cend(); ++it) {
        Backend* candidate = it.value().data();
        if (!candidate)
            continue;
        if (unique && unique != candidate)
            return nullptr;
        unique = candidate;
    }
    if (unique)
        return unique;

    // Preserve pre-login behavior for an unbound single-server widget.
    if (!_accountBackends.isEmpty())
        return nullptr;
    auto* mainWindow = qobject_cast<MainWindow*>(window());
    return mainWindow ? mainWindow->backendOrNull() : nullptr;
}

} // namespace Mattermost
