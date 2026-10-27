#include "WindowsUpdateController.h"

#ifdef Q_OS_WIN
#include "WindowsZipInstaller.h"
#include "UpdateTrustPolicy.h"
#include "options/MLOptions.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QSharedPointer>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QThread>
#include <QUrlQuery>
#include <QVersionNumber>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <appmodel.h>

namespace Mattermost {
namespace {
constexpr qint64 MaxPackage = 900LL * 1024 * 1024;
constexpr int MaxManifest = 128 * 1024;
constexpr int Protocol = 1;

bool safeIdentity(const QString& identity)
{
    static const QRegularExpression re(QStringLiteral("^[a-zA-Z0-9][a-zA-Z0-9._-]{0,89}$"));
    return re.match(identity).hasMatch() && identity != QLatin1String("..");
}

bool isHttpSuccess(QNetworkReply* reply)
{
    return reply->error() == QNetworkReply::NoError
        && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
        && reply->url().scheme() == QLatin1String("https");
}

QNetworkRequest requestFor(const QUrl& url)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Cache-Control", "no-cache, no-store");
    request.setRawHeader("User-Agent", "MatterLeast-Windows-Updater/1");
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setTransferTimeout(120000);
#endif
    return request;
}

bool packagedByStore()
{
    UINT32 length = 0;
    const LONG result = GetCurrentPackageFamilyName(&length, nullptr);
    return result != APPMODEL_ERROR_NO_PACKAGE;
}

QString activeChannel()
{
    const QString configured = MLOptions::instance()->value<QString>(
        QStringLiteral("updates/channel"), QStringLiteral("stable"));
    return configured == QLatin1String("nightly")
        ? QStringLiteral("nightly") : QStringLiteral("stable");
}
} // namespace

WindowsUpdateController::WindowsUpdateController(QObject* parent)
    : QObject(parent)
{
    // .../versions/<id>/MatterLeast/bin/matterleast.exe
    QDir path(QCoreApplication::applicationDirPath());
    const bool versioned = path.dirName() == QLatin1String("bin")
        && path.cdUp() && path.dirName() == QLatin1String("MatterLeast")
        && path.cdUp() && safeIdentity(path.dirName())
        && path.cdUp() && path.dirName() == QLatin1String("versions")
        && path.cdUp();
    if (!versioned || packagedByStore() || !updateChecksCompiled())
        return;

    _root = path.absolutePath();
    if (!installedLayout())
        return;

    setState(State::Idle);
    restorePrepared();

    _periodic.setInterval(6 * 60 * 60 * 1000);
    connect(&_periodic, &QTimer::timeout, this, [this] { check(true); });
    _periodic.start();
    QTimer::singleShot(20000, this, [this] { check(true); });
}

WindowsUpdateController::~WindowsUpdateController()
{
    if (_reply) _reply->abort();
    if (_packageFile.isOpen()) _packageFile.close();
}

bool WindowsUpdateController::installedLayout() const
{
    return QFileInfo::exists(QDir(_root).filePath(QStringLiteral("MatterLeastLauncher.exe")))
        && QFileInfo::exists(QDir(_root).filePath(QStringLiteral("versions")));
}

QString WindowsUpdateController::currentIdentity() const
{
    QFile pointer(QDir(_root).filePath(QStringLiteral("current.version")));
    if (!pointer.open(QIODevice::ReadOnly)) return {};
    const QString id = QString::fromUtf8(pointer.read(256)).trimmed();
    return safeIdentity(id) ? id : QString();
}

void WindowsUpdateController::setState(State state, const QString& error)
{
    _state = state;
    _error = error;
    emit stateChanged(state);
    if (state == State::Failed && !_automatic)
        emit errorOccurred(error);
}

void WindowsUpdateController::checkNow()
{
    check(false);
}

void WindowsUpdateController::check(bool automatic)
{
    if (!installedLayout() || !updateChecksCompiled()
        || _state == State::Unavailable || _state == State::Checking
        || _state == State::Downloading || _state == State::Preparing
        || _state == State::Ready || _reply)
        return;
    if (automatic && !MLOptions::instance()->value<bool>(
            QStringLiteral("updates/automatic"), true)) return;

    _automatic = automatic;
    setState(State::Checking);
    const auto channel = activeChannel() == QLatin1String("nightly")
        ? UpdateChannel::Nightly : UpdateChannel::Stable;
    const QUrl original(updateManifestUrl(channel));
    if (!original.isValid() || original.scheme() != QLatin1String("https")) {
        setState(State::Failed, tr("No safe update URL is configured"));
        return;
    }

    QUrl url = original;
    // GitHub Release assets are mutable: force a fresh pointer instead of a
    // cached asset URL from a previous update-channel promotion.
    if (url.host() == QLatin1String("github.com")) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("nocache"),
            QString::number(QDateTime::currentMSecsSinceEpoch()));
        url.setQuery(query);
    }
    downloadBytes(url, MaxManifest, [this](QByteArray json) {
        manifestReceived(json);
    });
}

void WindowsUpdateController::downloadBytes(
    const QUrl& url, int limit, const std::function<void(QByteArray)>& done)
{
    QNetworkReply* reply = _network.get(requestFor(url));
    _reply = reply;
    const auto collected = QSharedPointer<QByteArray>::create();
    connect(reply, &QIODevice::readyRead, this, [reply, collected, limit] {
        collected->append(reply->readAll());
        if (collected->size() > limit) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, collected, limit, done] {
        collected->append(reply->readAll());
        if (_reply == reply) _reply.clear();
        const bool valid = isHttpSuccess(reply) && collected->size() <= limit;
        const QString error = reply->errorString();
        reply->deleteLater();
        if (!valid) {
            setState(State::Failed, tr("Could not retrieve signed update metadata: %1").arg(error));
            return;
        }
        done(*collected);
    });
}

void WindowsUpdateController::manifestReceived(const QByteArray& raw)
{
    if (raw.isEmpty()) {
        setState(State::Failed, tr("Empty update manifest"));
        return;
    }
    if (!updateManifestSignatureRequired()) {
        authenticatedManifest(raw);
        return;
    }
    _rawManifest = raw;
    const auto channel = activeChannel() == QLatin1String("nightly")
        ? UpdateChannel::Nightly : UpdateChannel::Stable;
    const QUrl signatureUrl(updateManifestUrl(channel) + QStringLiteral(".sig"));
    downloadBytes(signatureUrl, 1024, [this](QByteArray signature) {
        if (!authenticateUpdateManifest(_rawManifest, signature)) {
            setState(State::Failed, tr("Update manifest signature verification failed"));
            return;
        }
        authenticatedManifest(_rawManifest);
        _rawManifest.clear();
    });
}

void WindowsUpdateController::authenticatedManifest(const QByteArray& raw)
{
    QJsonParseError parseError {};
    const QJsonDocument json = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        setState(State::Failed, tr("Invalid update manifest JSON"));
        return;
    }
    const QJsonObject doc = json.object();
    const QString channel = activeChannel();
    const QString version = doc.value(QStringLiteral("version")).toString();
    const QString tag = doc.value(QStringLiteral("releaseTag")).toString();
    const QString id = channel == QLatin1String("nightly")
        ? doc.value(QStringLiteral("buildId")).toString() : tag;
    const QJsonObject package = doc.value(QStringLiteral("package")).toObject();
    const QString format = package.value(QStringLiteral("format")).toString();
    const QString urlText = package.value(QStringLiteral("url")).toString();
    const QString sha = package.value(QStringLiteral("sha256")).toString();
    const double size = package.value(QStringLiteral("size")).toDouble(-1);
    const QUrl packageUrl(urlText);
    static const QRegularExpression ver(QStringLiteral("^\\d+\\.\\d+(?:\\.\\d+)?$"));
    static const QRegularExpression hash(QStringLiteral("^[a-f0-9]{64}$"));
    static const QRegularExpression nightly(QStringLiteral(
        "^nightly-\\d{8}-\\d{6}-[a-f0-9]{7,40}$"));
    if (doc.value(QStringLiteral("schema")).toInt(-1) != 1
        || doc.value(QStringLiteral("platform")).toString() != QLatin1String("windows-x64")
        || doc.value(QStringLiteral("channel")).toString() != channel
        || !ver.match(version).hasMatch()
        || doc.value(QStringLiteral("minimumLauncherProtocol")).toInt(999) > Protocol
        || doc.value(QStringLiteral("minimumLauncherProtocol")).toInt(0) < 1
        || !safeIdentity(id)
        || (channel == QLatin1String("stable") && tag != QStringLiteral("v") + version)
        || (channel == QLatin1String("nightly")
            && (tag != QLatin1String("updates-nightly") || !nightly.match(id).hasMatch()))
        || format != QLatin1String("portable-zip")
        || !packageUrl.isValid() || packageUrl.scheme() != QLatin1String("https")
        || !packageUrl.userInfo().isEmpty() || packageUrl.hasFragment()
        || !hash.match(sha).hasMatch() || size < 1024 || size > MaxPackage
        || size != qint64(size)) {
        setState(State::Failed, tr("Unsupported or unsafe update manifest"));
        return;
    }
    // The unsigned public build must not take binaries from arbitrary hosts.
    if (!updateManifestSignatureRequired()
        && !urlText.startsWith(
            QStringLiteral("https://github.com/Ri0n/matterleast/releases/download/"))) {
        setState(State::Failed, tr("Public updates must use the trusted GitHub release namespace"));
        return;
    }
    const QString installed = currentIdentity();
    if (installed.isEmpty()) {
        setState(State::Failed, tr("Missing installed version pointer"));
        return;
    }
    if (channel == QLatin1String("stable")) {
        if (QVersionNumber::compare(QVersionNumber::fromString(version),
            QVersionNumber::fromString(installed.startsWith(QLatin1Char('v'))
                ? installed.mid(1) : installed)) <= 0) {
            setState(State::Idle);
            return;
        }
    } else if (installed.startsWith(QStringLiteral("nightly-")) && id <= installed) {
        setState(State::Idle);
        return;
    }
    _identity = id;
    _size = qint64(size);
    _sha256 = sha;
    _packageUrl = packageUrl;
    const QString prepared = QDir(_root).filePath(
        QStringLiteral("versions/%1/MatterLeast/bin/MatterLeastUpdater.exe").arg(id));
    if (QFileInfo::exists(prepared)) {
        setState(State::Ready);
        emit updatePrepared(id);
        return;
    }
    beginPackageDownload();
}

void WindowsUpdateController::beginPackageDownload()
{
    const QString folder = QDir(_root).filePath(QStringLiteral("staging"));
    if (!QDir().mkpath(folder)) {
        setState(State::Failed, tr("Cannot create update staging directory"));
        return;
    }
    const QString finalPath = QDir(folder).filePath(_identity + QStringLiteral(".zip"));
    _packageFile.setFileName(finalPath + QStringLiteral(".part"));
    if (QFileInfo::exists(_packageFile.fileName())
        && !QFile::remove(_packageFile.fileName())) {
        setState(State::Failed, tr("Cannot remove interrupted package download"));
        return;
    }
    if (!_packageFile.open(QIODevice::WriteOnly)) {
        setState(State::Failed, tr("Cannot write downloaded update package"));
        return;
    }
    _received = 0;
    _digest.reset();
    setState(State::Downloading);
    QNetworkReply* reply = _network.get(requestFor(_packageUrl));
    _reply = reply;

    connect(reply, &QIODevice::readyRead, this, [this, reply] {
        const QByteArray data = reply->readAll();
        _received += data.size();
        if (_received > _size || _packageFile.write(data) != data.size()) {
            reply->abort();
            return;
        }
        _digest.addData(data);
        emit progressChanged(_received, _size);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, finalPath] {
        if (_reply == reply) _reply.clear();
        const QByteArray last = reply->readAll();
        if (!last.isEmpty()) {
            _received += last.size();
            if (_received <= _size && _packageFile.write(last) == last.size())
                _digest.addData(last);
        }
        const bool ok = isHttpSuccess(reply) && _received == _size
            && QString::fromLatin1(_digest.result().toHex()) == _sha256
            && _packageFile.flush();
        reply->deleteLater();
        _packageFile.close();
        if (!ok || !QFile::rename(_packageFile.fileName(), finalPath)) {
            QFile::remove(_packageFile.fileName());
            setState(State::Failed, tr("Downloaded ZIP failed size, SHA-256 or I/O verification"));
            return;
        }
        startPreparing(finalPath);
    });
}

void WindowsUpdateController::startPreparing(const QString& verifiedPackagePath)
{
    setState(State::Preparing);
    const QString root = _root, identity = _identity;
    const QPointer<WindowsUpdateController> self(this);
    QThread* thread = QThread::create([self, verifiedPackagePath, root, identity] {
        QString reason;
        const bool ok = prepareWindowsUpdateZip(verifiedPackagePath, root, identity, &reason);
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, identity, ok, reason] {
            if (!self || self->_identity != identity) return;
            if (!ok) {
                self->setState(State::Failed, reason);
                return;
            }
            QJsonObject manifest {
                { QStringLiteral("identity"), identity },
                { QStringLiteral("sha256"), self->_sha256 }
            };
            QSaveFile output(QDir(self->_root).filePath(QStringLiteral("staging/prepared.json")));
            if (!output.open(QIODevice::WriteOnly)
                || output.write(QJsonDocument(manifest).toJson(QJsonDocument::Compact)) < 0
                || !output.commit()) {
                self->setState(State::Failed, self->tr("Cannot preserve prepared update state"));
                return;
            }
            self->setState(State::Ready);
            emit self->updatePrepared(identity);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void WindowsUpdateController::restorePrepared()
{
    QFile file(QDir(_root).filePath(QStringLiteral("staging/prepared.json")));
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(4096));
    if (!doc.isObject()) return;
    const QString id = doc.object().value(QStringLiteral("identity")).toString();
    if (!safeIdentity(id) || id == currentIdentity()) return;
    if (!QFileInfo::exists(QDir(_root).filePath(
            QStringLiteral("versions/%1/MatterLeast/bin/MatterLeastUpdater.exe").arg(id)))) return;
    _identity = id;
    setState(State::Ready);
    emit updatePrepared(id);
}

void WindowsUpdateController::startUpdateAndRestart()
{
    if (!canApply() || !safeIdentity(_identity) || _root.isEmpty()) return;
    const QString exe = QDir(_root).filePath(
        QStringLiteral("versions/%1/MatterLeast/bin/MatterLeastUpdater.exe").arg(_identity));
    if (!QFileInfo::exists(exe)) {
        setState(State::Failed, tr("Prepared update helper has disappeared"));
        return;
    }
    const QStringList args = {
        QStringLiteral("--root"), _root,
        QStringLiteral("--target"), _identity,
        QStringLiteral("--wait-pid"),
        QString::number(QCoreApplication::applicationPid())
    };
    if (!QProcess::startDetached(exe, args, QFileInfo(exe).absolutePath())) {
        setState(State::Failed, tr("Could not launch version switcher"));
        return;
    }
    QCoreApplication::quit();
}

} // namespace Mattermost
#endif // Q_OS_WIN
