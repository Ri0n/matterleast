#include "ChannelTree.h"

#include "backend/Backend.h"

#include "mainwindow.h"

namespace Mattermost {

Backend* ChannelTree::backendInstance() const
{
    if (backendForSidebar) {
        return backendForSidebar;
    }

    auto* mainWindow = qobject_cast<MainWindow*>(window());
    return mainWindow ? &mainWindow->backendInstance() : nullptr;
}

} // namespace Mattermost
