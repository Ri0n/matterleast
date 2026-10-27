#include "WindowsZipInstaller.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QStringList>
#include <QtEndian>

namespace Mattermost {
namespace {
constexpr qint64 MaxPackageBytes = 900LL * 1024 * 1024;
constexpr quint64 MaxUnpackedBytes = 3ULL * 1024 * 1024 * 1024;
constexpr int MaxFiles = 30000;
constexpr quint32 LocalHeader = 0x04034b50;
constexpr quint32 CentralHeader = 0x02014b50;
constexpr quint32 EndHeader = 0x06054b50;

quint16 word(const char* p) { return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(p)); }
quint32 dword(const char* p) { return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(p)); }

bool validWindowsPart(const QString& name)
{
    if (name.isEmpty() || name.size() > 240 || name == QLatin1String(".")
        || name == QLatin1String("..") || name.endsWith(QLatin1Char('.'))
        || name.endsWith(QLatin1Char(' '))) return false;
    for (const QChar ch : name) {
        if (ch.unicode() < 32 || QStringLiteral("<>:|?*\"\\").contains(ch)) return false;
    }
    const QString base = name.section(QLatin1Char('.'), 0, 0).toUpper();
    if (base == QLatin1String("CON") || base == QLatin1String("PRN")
        || base == QLatin1String("AUX") || base == QLatin1String("NUL"))
        return false;
    if (base.size() == 4
        && (base.startsWith(QLatin1String("COM")) || base.startsWith(QLatin1String("LPT")))
        && base[3].unicode() >= '1' && base[3].unicode() <= '9') return false;
    return true;
}

// Parse only the central ZIP directory. CPack's PowerShell-generated archives
// use this standard layout. ZIP64/multidisk/symlinks are intentionally refused.
bool auditZip(const QString& filename, QString* error)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 22
        || file.size() > MaxPackageBytes) {
        *error = QStringLiteral("ZIP is unreadable or exceeds allowed size");
        return false;
    }
    const qint64 start = qMax<qint64>(0, file.size() - 65557);
    if (!file.seek(start)) return false;
    const QByteArray tail = file.readAll();
    int eocd = -1;
    for (int i = tail.size() - 22; i >= 0; --i) {
        if (dword(tail.constData() + i) == EndHeader
            && word(tail.constData() + i + 20) == tail.size() - i - 22) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        *error = QStringLiteral("ZIP directory is missing");
        return false;
    }
    const char* end = tail.constData() + eocd;
    const quint16 count = word(end + 10);
    const quint32 directorySize = dword(end + 12);
    const quint32 directoryOffset = dword(end + 16);
    if (word(end + 4) || word(end + 6) || word(end + 8) != count
        || count == 0 || count > MaxFiles || directorySize > 16 * 1024 * 1024
        || directoryOffset == 0xffffffffu || directorySize == 0xffffffffu
        || quint64(directoryOffset) + directorySize > quint64(file.size())) {
        *error = QStringLiteral("Unsupported ZIP directory");
        return false;
    }
    if (!file.seek(directoryOffset)) return false;
    const QByteArray directory = file.read(directorySize);
    if (directory.size() != qsizetype(directorySize)) return false;

    quint64 expanded = 0;
    QSet<QString> unique;
    bool launcher = false, application = false, updater = false;
    int pos = 0;
    for (int i = 0; i < count; ++i) {
        if (directory.size() - pos < 46 || dword(directory.constData() + pos) != CentralHeader) {
            *error = QStringLiteral("Malformed ZIP central directory");
            return false;
        }
        const char* row = directory.constData() + pos;
        const quint16 flags = word(row + 8);
        const quint16 method = word(row + 10);
        const quint32 uncompressed = dword(row + 24);
        const quint16 nameSize = word(row + 28), extraSize = word(row + 30),
                      commentSize = word(row + 32);
        const quint32 external = dword(row + 38);
        const quint32 localOffset = dword(row + 42);
        if ((flags & 1) || (method != 0 && method != 8)
            || uncompressed == 0xffffffffu || localOffset == 0xffffffffu
            || pos + 46 + nameSize + extraSize + commentSize > directory.size()) {
            *error = QStringLiteral("Unsafe ZIP entry or unsupported ZIP64/encryption");
            return false;
        }
        // The Unix file type is in the upper mode bits of external attributes.
        // Deny links and other special files even if bsdtar could create them.
        const quint32 type = (external >> 16) & 0170000;
        if (type != 0 && type != 0100000 && type != 0040000) {
            *error = QStringLiteral("ZIP contains a link or special file");
            return false;
        }
        const QByteArray encoded(row + 46, nameSize);
        const QString name = (flags & (1 << 11))
            ? QString::fromUtf8(encoded) : QString::fromLatin1(encoded);
        if (name.isEmpty() || name.contains(QChar::ReplacementCharacter)
            || name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))) {
            *error = QStringLiteral("ZIP path encoding or separator rejected");
            return false;
        }
        const bool directoryEntry = name.endsWith(QLatin1Char('/'));
        const QString trimmed = directoryEntry ? name.left(name.size() - 1) : name;
        const QStringList parts = trimmed.split(QLatin1Char('/'), Qt::KeepEmptyParts);
        if (parts.isEmpty() || parts.first() != QLatin1String("MatterLeast")) {
            *error = QStringLiteral("ZIP entries must reside under MatterLeast/");
            return false;
        }
        for (const QString& part : parts) {
            if (!validWindowsPart(part)) {
                *error = QStringLiteral("Unsafe ZIP entry path: %1").arg(name);
                return false;
            }
        }
        const QString folded = trimmed.toCaseFolded();
        if (unique.contains(folded)) {
            *error = QStringLiteral("Duplicate ZIP path: %1").arg(name);
            return false;
        }
        unique.insert(folded);
        expanded += uncompressed;
        if (expanded > MaxUnpackedBytes) {
            *error = QStringLiteral("ZIP extraction size limit exceeded");
            return false;
        }
        if (name == QLatin1String("MatterLeast/MatterLeastLauncher.exe")) launcher = true;
        if (name.endsWith(QLatin1String("/MatterLeast/bin/matterleast.exe"))) application = true;
        if (name.endsWith(QLatin1String("/MatterLeast/bin/MatterLeastUpdater.exe"))) updater = true;
        pos += 46 + nameSize + extraSize + commentSize;
    }
    if (pos != directory.size() || !launcher || !application || !updater) {
        *error = QStringLiteral("Missing launcher/application/updater or trailing ZIP metadata");
        return false;
    }
    return true;
}

bool subtreeWithoutLinks(const QString& root)
{
    // No symlinks or reparse points are allowed in the version payload.
    QDir dir(root);
    if (!dir.exists()) return false;
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot
                                                   | QDir::Hidden | QDir::System);
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink() || !entry.isReadable()) return false;
        if (entry.isDir() && !subtreeWithoutLinks(entry.absoluteFilePath())) return false;
    }
    return true;
}
} // namespace

bool auditWindowsUpdateZip(const QString& zipPath, QString* error)
{
    return auditZip(zipPath, error);
}

bool prepareWindowsUpdateZip(const QString& zipPath, const QString& root,
                             const QString& identity, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error) *error = why;
        return false;
    };
    if (identity.isEmpty() || identity.size() > 90) return fail(QStringLiteral("Invalid update identity"));
    for (const QChar ch : identity) {
        if (!ch.isLetterOrNumber() && ch != QLatin1Char('-')
            && ch != QLatin1Char('_') && ch != QLatin1Char('.'))
            return fail(QStringLiteral("Invalid update identity"));
    }
    QString problem;
    if (!auditZip(zipPath, &problem)) return fail(problem);
    QDir installation(root);
    const QString stage = installation.filePath(QStringLiteral("staging/%1.extracted").arg(identity));
    if (QFileInfo::exists(stage) && !QDir(stage).removeRecursively())
        return fail(QStringLiteral("Cannot clear interrupted update staging"));
    if (!QDir().mkpath(stage)) return fail(QStringLiteral("Cannot create update staging"));

    QProcess tar;
    tar.start(QStringLiteral("tar.exe"), {QStringLiteral("-xf"), zipPath,
              QStringLiteral("-C"), stage});
    if (!tar.waitForStarted(10000) || !tar.waitForFinished(180000)
        || tar.exitStatus() != QProcess::NormalExit || tar.exitCode() != 0
        || !subtreeWithoutLinks(stage)) {
        QDir(stage).removeRecursively();
        return fail(QStringLiteral("Windows ZIP extraction failed or contained unsafe file types"));
    }

    const QString versions = QDir(stage).filePath(QStringLiteral("MatterLeast/versions"));
    const QStringList dirs = QDir(versions).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (dirs.size() != 1) {
        QDir(stage).removeRecursively();
        return fail(QStringLiteral("Update ZIP does not contain exactly one version"));
    }
    const QString runtime = QDir(versions).filePath(dirs.first() + QStringLiteral("/MatterLeast"));
    if (!QFileInfo::exists(QDir(runtime).filePath(QStringLiteral("bin/matterleast.exe")))
        || !QFileInfo::exists(QDir(runtime).filePath(QStringLiteral("bin/MatterLeastUpdater.exe")))
        || !QFileInfo::exists(QDir(runtime).filePath(QStringLiteral("bin/Qt6Core.dll")))) {
        QDir(stage).removeRecursively();
        return fail(QStringLiteral("Update ZIP is missing mandatory application files"));
    }

    const QString versionsRoot = installation.filePath(QStringLiteral("versions"));
    if (!QDir().mkpath(versionsRoot)) return fail(QStringLiteral("Cannot prepare version directory"));
    const QString dest = QDir(versionsRoot).filePath(identity);
    const QString temporary = dest + QStringLiteral(".partial");
    if (QFileInfo::exists(dest)) {
        QDir(stage).removeRecursively();
        return QFileInfo::exists(QDir(dest).filePath(QStringLiteral("MatterLeast/bin/matterleast.exe")))
            || fail(QStringLiteral("Prepared version already exists but is incomplete"));
    }
    if (QFileInfo::exists(temporary) && !QDir(temporary).removeRecursively())
        return fail(QStringLiteral("Interrupted version directory could not be removed"));
    if (!QDir().rename(runtime.left(runtime.size() - QStringLiteral("/MatterLeast").size()), temporary)) {
        QDir(stage).removeRecursively();
        return fail(QStringLiteral("Cannot move version payload into install root"));
    }
    if (!QDir().rename(temporary, dest)) return fail(QStringLiteral("Cannot atomically finalize version"));
    QDir(stage).removeRecursively();
    return true;
}
} // namespace Mattermost
