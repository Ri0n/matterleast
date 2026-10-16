#pragma once

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QSize>
#include <QTemporaryDir>
#include <QUrl>

namespace Mattermost::EmojiRaster {

// Render custom images at their actual presentation size using a high-quality
// resampler. Qt's default rich-text image path may downscale with the painter's
// fast transformation, which leaves visible stair-steps on small reactions.
// Files are kept only for this process lifetime: no stale thumbnails, no
// persistent duplicate custom-emoji cache and no additional HTTP requests.
inline QString sourcePath(QString source)
{
    if (source.startsWith(QStringLiteral("qrc://"))) {
        return QStringLiteral(":/") + source.mid(6);
    }
    if (source.startsWith(QStringLiteral("qrc:/"))) {
        return QStringLiteral(":/") + source.mid(5);
    }
    const QUrl url(source);
    return url.isLocalFile() ? url.toLocalFile() : source;
}

inline QString smoothedPath(const QString& source, const QSize& size)
{
    if (source.isEmpty() || !size.isValid() || size.isEmpty()) {
        return {};
    }
    const QString path = sourcePath(source);
    // Only local/cache files and packaged resources are eligible. Never turn
    // an arbitrary URL from server-provided HTML into a network image fetch.
    if (path.contains(QStringLiteral("://"))
        || (!QFileInfo::exists(path) && !path.startsWith(QStringLiteral(":/")))) {
        return {};
    }

    QImageReader reader(path);
    // The server's emoji cache may use a .gif extension for a PNG/JPEG.
    reader.setDecideFormatFromContent(true);
    const QSize native = reader.size();
    if (!native.isValid() || native.isEmpty()
        || (native.width() <= size.width() && native.height() <= size.height())) {
        return {};
    }

    const QSize target = native.scaled(size, Qt::KeepAspectRatio);
    if (target == native) {
        return {};
    }

    static QTemporaryDir temporary(
        QDir::tempPath() + QStringLiteral("/matterleast-emoji-XXXXXX"));
    if (!temporary.isValid()) {
        return {};
    }

    const QFileInfo info(path);
    const QByteArray identity = path.toUtf8() + '\n'
        + QByteArray::number(info.size()) + '\n'
        + QByteArray::number(info.lastModified().toMSecsSinceEpoch()) + '\n'
        + QByteArray::number(target.width()) + 'x'
        + QByteArray::number(target.height());
    const QString name = QString::fromLatin1(
        QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex())
        + QStringLiteral(".png");
    const QString output = temporary.filePath(name);
    if (QFileInfo::exists(output)) {
        return output;
    }

    const QImage image = reader.read();
    if (image.isNull()) {
        return {};
    }
    const QImage scaled = image.scaled(
        target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return scaled.save(output, "PNG") ? output : QString();
}

inline QString smoothedUrl(const QString& source, const QSize& size)
{
    const QString path = smoothedPath(source, size);
    return path.isEmpty() ? QString()
                          : QUrl::fromLocalFile(path).toString();
}

} // namespace Mattermost::EmojiRaster
