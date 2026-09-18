#pragma once

#include <QIcon>
#include <QString>

namespace Mattermost::AttachmentPresentation {

struct FilePresentation {
    QString mimeTypeName;
    QIcon icon;
};

FilePresentation describeFile(const QString& fileName);

} // namespace Mattermost::AttachmentPresentation
