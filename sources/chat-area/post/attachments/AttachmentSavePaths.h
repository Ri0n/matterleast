#pragma once

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QString>

namespace Mattermost::AttachmentSavePaths {

// Treat server-provided names as labels, never as filesystem paths.
inline QString safeName(QString name)
{
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    name = name.section(QLatin1Char('/'), -1).trimmed();
    for (QChar& character : name) {
        if (character.unicode() < 32 || QStringLiteral("<>:"/\\|?*").contains(character)) {
            character = QLatin1Char('_');
        }
    }
    while (name.endsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char(' '))) {
        name.chop(1);
    }
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        name = QStringLiteral("attachment");
    }
    // Windows reserves device names even when followed by an extension.
    const QString basename = name.section(QLatin1Char('.'), 0, 0).toUpper();
    if (basename == QLatin1String("CON") || basename == QLatin1String("PRN")
        || basename == QLatin1String("AUX") || basename == QLatin1String("NUL")
        || (basename.size() == 4 && (basename.startsWith(QLatin1String("COM"))
                                     || basename.startsWith(QLatin1String("LPT")))
            && basename.at(3) >= QLatin1Char('1')
            && basename.at(3) <= QLatin1Char('9'))) {
        name.prepend(QLatin1Char('_'));
    }
    return name;
}

// Never silently replace an existing download; disambiguate duplicate names
// both on disk and within the current batch.
inline QString unusedPath(const QDir& directory, const QString& remoteName,
                          const QSet<QString>& reserved = {})
{
    const QString name = safeName(remoteName);
    const int dot = name.lastIndexOf(QLatin1Char('.'));
    const QString stem = dot > 0 ? name.left(dot) : name;
    const QString suffix = dot > 0 ? name.mid(dot) : QString();
    for (int number = 0;; ++number) {
        const QString candidate = number == 0
            ? name : stem + QStringLiteral(" (%1)").arg(number) + suffix;
        const QString fullPath = directory.filePath(candidate);
        bool alreadyReserved = false;
        for (const QString& value : reserved) {
            if (value.compare(candidate, Qt::CaseInsensitive) == 0) {
                alreadyReserved = true;
                break;
            }
        }
        if (!alreadyReserved && !QFileInfo::exists(fullPath)) {
            return fullPath;
        }
    }
}

} // namespace Mattermost::AttachmentSavePaths
