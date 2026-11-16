#pragma once

#include <QString>

class QWidget;

namespace Mattermost {
class Backend;

// One MatterLeast-owned save path for images, video and audio. The dialog
// follows the configured downloads folder; the transfer uses the authenticated
// attachment service and never depends on a message widget's lifetime.
class AttachmentSaveFile
{
public:
    static void chooseAndSave(Backend& backend, const QString& fileId,
                              const QString& fileName, QWidget* parent = nullptr,
                              const QString& cachedPath = {});
    static void saveToPath(Backend& backend, const QString& fileId,
                           const QString& destination,
                           QWidget* notificationParent = nullptr,
                           const QString& cachedPath = {});
};
} // namespace Mattermost
