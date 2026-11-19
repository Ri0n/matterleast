#pragma once

#include <QString>
#include <QStringList>

namespace Mattermost {

// systemRelativeDirectory is empty on platforms whose plugins live beside
// the executable. On Linux it is the GNUInstallDirs module path relative to
// the installed bindir. The executable-local directory is then eligible only
// when running the executable directly from its build output directory.
QStringList resolveNativePluginSearchPaths(
    const QString& executableDirectory,
    const QString& buildExecutableDirectory,
    const QString& systemRelativeDirectory,
    const QString& userDataDirectory);

} // namespace Mattermost
