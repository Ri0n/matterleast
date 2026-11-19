#include "PluginSearchPathPolicy.h"

#include <QDir>

namespace Mattermost {

QStringList resolveNativePluginSearchPaths(
    const QString& executableDirectory,
    const QString& buildExecutableDirectory,
    const QString& systemRelativeDirectory,
    const QString& userDataDirectory)
{
    const QString executableDir =
        QDir::cleanPath(QDir(executableDirectory).absolutePath());
    const bool systemLayout = !systemRelativeDirectory.isEmpty();
    const QString buildDir = buildExecutableDirectory.isEmpty()
        ? QString()
        : QDir::cleanPath(QDir(buildExecutableDirectory).absolutePath());

    QStringList paths;

    // On Windows and macOS plugins are shipped beside the executable. Linux
    // packages use libdir instead: /usr/bin/plugins is not a MatterLeast
    // plugin directory and must neither be advertised nor scanned.
    // An uninstalled developer build is the sole Linux exception.
    if (!systemLayout || (!buildDir.isEmpty() && executableDir == buildDir)) {
        paths.push_back(QDir(executableDir).filePath(QStringLiteral("plugins")));
    }

    if (systemLayout) {
        const QString systemPlugins = QDir::cleanPath(
            QDir(executableDir).filePath(systemRelativeDirectory));
        if (!paths.contains(systemPlugins)) {
            paths.push_back(systemPlugins);
        }
    }

    if (!userDataDirectory.isEmpty()) {
        const QString userPlugins = QDir(userDataDirectory).filePath(
            QStringLiteral("plugins"));
        if (!paths.contains(userPlugins)) {
            paths.push_back(userPlugins);
        }
    }

    return paths;
}

} // namespace Mattermost
