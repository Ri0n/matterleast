#pragma once

class QApplication;

namespace Mattermost {

// Install font defaults/migrations and observe interface previews until shutdown.
void initializeApplicationFont(QApplication& app);

} // namespace Mattermost
