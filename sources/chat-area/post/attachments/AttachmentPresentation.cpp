#include "AttachmentPresentation.h"

#include <utility>

#include <QApplication>
#include <QMimeDatabase>
#include <QMimeType>
#include <QStyle>

namespace Mattermost::AttachmentPresentation {

FilePresentation describeFile(const QString& fileName)
{
    static QMimeDatabase mimeDatabase;

    const QMimeType mimeType =
        mimeDatabase.mimeTypeForFile(fileName, QMimeDatabase::MatchExtension);

    QIcon icon = QIcon::fromTheme(mimeType.iconName());
    if (icon.isNull()) {
        icon = QIcon::fromTheme(mimeType.genericIconName());
    }
    if (icon.isNull()) {
        icon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    }

    return {mimeType.name(), std::move(icon)};
}

} // namespace Mattermost::AttachmentPresentation
