#pragma once

#include <QString>

namespace Mattermost {
// Validate ZIP central directory before using Windows bsdtar, and extract
// only into a disposable directory. Symlinks, traversals, absolute names,
// duplicate case-insensitive paths, encrypted entries and zip bombs are denied.
bool auditWindowsUpdateZip(const QString& zipPath, QString* error);
bool prepareWindowsUpdateZip(const QString& zipPath, const QString& root,
                             const QString& identity, QString* error);
}
