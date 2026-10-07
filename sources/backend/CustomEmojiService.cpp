/*
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "CustomEmojiService.h"

#include <algorithm>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVector>

#include "Backend.h"
#include "NetworkRequest.h"
#include "QByteArrayCreator.h"

namespace Mattermost {
namespace {

constexpr int MaxNamesPerBatch = 200;
constexpr int MaxRememberedMissingNames = 2048;
constexpr qint64 MaxCustomEmojiDiskCacheBytes =
    128LL * 1024 * 1024;

QString normalizedServerCacheIdentity(const Backend& backend)
{
    QString identity = backend.serverDomain().trimmed();
    if (identity.isEmpty()) {
        return {};
    }

    QUrl url(identity);
    if (url.isValid() && !url.host().isEmpty()) {
        url.setScheme(url.scheme().toLower());
        url.setHost(url.host().toLower());
        url.setQuery(QString());
        url.setFragment(QString());
        identity = url.toString(QUrl::FullyEncoded);
    }
    while (identity.endsWith(QLatin1Char('/'))) {
        identity.chop(1);
    }
    return identity;
}

QString customEmojiCacheRootPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
        .filePath(QStringLiteral("custom-emoji"));
}

QString customEmojiServerCachePath(const Backend& backend)
{
    const QString identity = normalizedServerCacheIdentity(backend);
    if (identity.isEmpty()) {
        return {};
    }

    const QByteArray digest = QCryptographicHash::hash(
        identity.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    return QDir(customEmojiCacheRootPath())
        .filePath(QString::fromLatin1(digest));
}

void touchCustomEmojiCacheFile(const QString& path)
{
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
        file.setFileTime(
            QDateTime::currentDateTimeUtc(),
            QFileDevice::FileModificationTime);
    }
}

void removeLegacyUnscopedEmojiCache()
{
    QDir root(customEmojiCacheRootPath());
    if (!root.exists()) {
        return;
    }

    const QFileInfoList legacyFiles =
        root.entryInfoList(QDir::Files | QDir::NoSymLinks);
    for (const QFileInfo& file : legacyFiles) {
        QFile::remove(file.absoluteFilePath());
    }
}

void pruneCustomEmojiDiskCache()
{
    const QString rootPath = customEmojiCacheRootPath();
    QDir root(rootPath);
    if (!root.exists()) {
        return;
    }

    struct CacheFile {
        QString path;
        qint64 size = 0;
        QDateTime lastUsed;
    };

    QVector<CacheFile> files;
    qint64 totalBytes = 0;
    QDirIterator it(
        rootPath,
        QDir::Files | QDir::NoSymLinks,
        QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo info(path);
        if (!info.isFile()) {
            continue;
        }
        files.push_back(CacheFile {
            path,
            info.size(),
            info.lastModified(),
        });
        totalBytes += info.size();
    }

    if (totalBytes <= MaxCustomEmojiDiskCacheBytes) {
        return;
    }

    std::sort(files.begin(), files.end(),
              [](const CacheFile& left, const CacheFile& right) {
        return left.lastUsed < right.lastUsed;
    });
    for (const CacheFile& file : files) {
        if (totalBytes <= MaxCustomEmojiDiskCacheBytes) {
            break;
        }
        if (QFile::remove(file.path)) {
            totalBytes -= file.size;
        }
    }
}

bool isUnsupportedBatchStatus(int status)
{
    return status == 404 || status == 405 || status == 501;
}

} // namespace

CustomEmojiService& CustomEmojiService::instance(Backend& backend)
{
    if (auto* service = backend.findChild<CustomEmojiService*>(
            QString(), Qt::FindDirectChildrenOnly)) {
        return *service;
    }
    return *new CustomEmojiService(backend);
}

CustomEmojiService::CustomEmojiService(Backend& backend)
    : QObject(&backend)
    , _backend(backend)
    , _missingNames(MaxRememberedMissingNames)
{
    removeLegacyUnscopedEmojiCache();
    pruneCustomEmojiDiskCache();
    connect(&_backend.emojiRegistry(),
            &EmojiRegistry::customEmojiRequested,
            this, &CustomEmojiService::ensureEmoji);
    connect(&_backend.emojiRegistry(),
            &EmojiRegistry::customEmojiAdded,
            this, [this](const QString& name) {
        // Search, on-demand browsing and lazy per-name resolution can race.
        // Once any path registers the image, suppress stale queued work and
        // clear any previous negative result for that name.
        _pendingNames.remove(name);
        _inFlightNames.remove(name);
        _missingNames.remove(name);
    });

    // Requests owned by this best-effort background resolver are deliberately
    // not forwarded to Backend::onNetworkError. A perfectly ordinary literal
    // such as :not_an_emoji: may resolve to HTTP 404 and must remain silent.
    // Clear negative/transient state on reconnect so new server-side emoji and
    // cancelled requests become eligible for lookup again.
}

void CustomEmojiService::resetSession()
{
    _httpConnector.reset();
    _pendingNames.clear();
    _inFlightNames.clear();
    _missingNames.clear();
    _searchesInFlight.clear();
    _flushScheduled = false;
    _batchLookupSupported = true;
    _browsePageRequested = false;
}

bool CustomEmojiService::isValidCustomEmojiName(const QString& name)
{
    static const QRegularExpression expression(
        QStringLiteral(R"(^[A-Za-z0-9_+\-]+$)"));
    return !name.isEmpty() && expression.match(name).hasMatch();
}

void CustomEmojiService::ensureEmoji(const QString& name)
{
    // EmojiRegistry emits customEmojiRequested only after its local lookup misses,
    // so looking it up again here would recurse back into this slot.
    if (!isValidCustomEmojiName(name)
        || _pendingNames.contains(name)
        || _inFlightNames.contains(name)
        || _missingNames.contains(name)) {
        return;
    }

    _pendingNames.insert(name);
    if (_flushScheduled) {
        return;
    }

    _flushScheduled = true;
    QTimer::singleShot(0, this, [this] {
        flushPendingNames();
    });
}

void CustomEmojiService::searchEmojis(const QString& term)
{
    const QString search = term.trimmed();
    if (search.isEmpty() || _searchesInFlight.contains(search)) {
        return;
    }

    _searchesInFlight.insert(search);

    QJsonObject body;
    body.insert(QStringLiteral("term"), search);

    NetworkRequest request(QStringLiteral("emoji/search"));
    _httpConnector.post(request, QByteArrayCreator(body),
                        HttpResponseCallback(
        [this, search](const QJsonDocument& doc, const QNetworkReply& reply) {
            _searchesInFlight.remove(search);
            if (reply.error() != QNetworkReply::NoError) {
                return;
            }

            for (const QJsonValue& value : doc.array()) {
                const QJsonObject object = value.toObject();
                const QString id = object.value(QStringLiteral("id")).toString();
                const QString name = object.value(QStringLiteral("name")).toString();
                if (id.isEmpty() || name.isEmpty()
                    || !isValidCustomEmojiName(name)) {
                    continue;
                }

                _missingNames.remove(name);
                _pendingNames.remove(name);
                if (_inFlightNames.contains(name)) {
                    continue;
                }

                _inFlightNames.insert(name);
                ensureImage(id, name);
            }
        }));
}

void CustomEmojiService::ensureBrowsePageLoaded()
{
    if (_browsePageRequested) {
        return;
    }
    _browsePageRequested = true;

    NetworkRequest request(QStringLiteral("emoji?page=0&per_page=60"));
    _httpConnector.get(request, HttpResponseCallback(
        [this](const QJsonDocument& doc, const QNetworkReply& reply) {
            if (reply.error() != QNetworkReply::NoError) {
                _browsePageRequested = false;
                return;
            }

            for (const QJsonValue& value : doc.array()) {
                const QJsonObject object = value.toObject();
                const QString id = object.value(QStringLiteral("id")).toString();
                const QString name = object.value(QStringLiteral("name")).toString();
                if (id.isEmpty() || name.isEmpty()
                    || !isValidCustomEmojiName(name)) {
                    continue;
                }

                _missingNames.remove(name);
                _pendingNames.remove(name);
                if (_inFlightNames.contains(name)) {
                    continue;
                }

                _inFlightNames.insert(name);
                ensureImage(id, name);
            }
        }));
}

void CustomEmojiService::flushPendingNames()
{
    _flushScheduled = false;
    if (_pendingNames.isEmpty()) {
        return;
    }

    QSet<QString> requested;
    auto it = _pendingNames.begin();
    while (it != _pendingNames.end() && requested.size() < MaxNamesPerBatch) {
        requested.insert(*it);
        it = _pendingNames.erase(it);
    }
    _inFlightNames.unite(requested);

    if (!_pendingNames.isEmpty()) {
        _flushScheduled = true;
        QTimer::singleShot(0, this, [this] {
            flushPendingNames();
        });
    }

    if (!_batchLookupSupported) {
        lookupNamesIndividually(requested);
        return;
    }

    QJsonArray names;
    for (const QString& name : requested) {
        names.push_back(name);
    }

    NetworkRequest request(QStringLiteral("emoji/names"));
    _httpConnector.post(request, QByteArrayCreator(names),
                        HttpResponseCallback(
        [this, requested](const QJsonDocument& doc, const QNetworkReply& reply) {
            if (reply.error() != QNetworkReply::NoError) {
                const int httpStatus = reply.attribute(
                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (isUnsupportedBatchStatus(httpStatus)) {
                    // POST /emoji/names was added in Mattermost 9.2. Fall back
                    // to the per-name endpoint available since 4.7 and remember
                    // that decision for the rest of this connection.
                    _batchLookupSupported = false;
                    lookupNamesIndividually(requested);
                    return;
                }

                for (const QString& name : requested) {
                    _inFlightNames.remove(name);
                }
                return;
            }

            QSet<QString> found;
            for (const QJsonValue& value : doc.array()) {
                const QJsonObject object = value.toObject();
                const QString id = object.value(QStringLiteral("id")).toString();
                const QString name = object.value(QStringLiteral("name")).toString();
                if (id.isEmpty() || name.isEmpty() || !requested.contains(name)) {
                    continue;
                }

                found.insert(name);
                // Keep the name in-flight until its cached or downloaded image
                // has actually been registered in the backend emoji registry.
                ensureImage(id, name);
            }

            for (const QString& name : requested) {
                if (found.contains(name)) {
                    continue;
                }
                _inFlightNames.remove(name);
                _missingNames.insert(name, new char(0));
            }
        }));
}

void CustomEmojiService::lookupNamesIndividually(const QSet<QString>& names)
{
    for (const QString& requestedName : names) {
        NetworkRequest request(QStringLiteral("emoji/name/") + requestedName);
        _httpConnector.get(request, HttpResponseCallback(
            [this, requestedName](const QJsonDocument& doc, const QNetworkReply& reply) {
                if (reply.error() != QNetworkReply::NoError) {
                    const int httpStatus = reply.attribute(
                        QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    _inFlightNames.remove(requestedName);
                    if (httpStatus == 404) {
                        _missingNames.insert(requestedName, new char(0));
                    }
                    return;
                }

                const QJsonObject object = doc.object();
                const QString id = object.value(QStringLiteral("id")).toString();
                const QString name = object.value(QStringLiteral("name")).toString();
                if (id.isEmpty() || name.isEmpty()) {
                    _inFlightNames.remove(requestedName);
                    return;
                }

                ensureImage(id, name);
            }));
    }
}

void CustomEmojiService::ensureImage(const QString& id, const QString& name)
{
    const QString serverCachePath = customEmojiServerCachePath(_backend);
    if (serverCachePath.isEmpty()) {
        _inFlightNames.remove(name);
        return;
    }

    QDir emojiDir(serverCachePath);
    if (!emojiDir.exists() && !emojiDir.mkpath(QStringLiteral("."))) {
        _inFlightNames.remove(name);
        return;
    }

    // The .gif suffix is historical; Qt image readers identify PNG/JPEG/GIF
    // data by content. The server hash prevents identical Mattermost emoji IDs
    // from colliding across backends.
    const QString filePath = emojiDir.filePath(id + QStringLiteral(".gif"));
    const QFileInfo cached(filePath);
    if (cached.exists() && cached.isFile() && cached.size() > 0) {
        touchCustomEmojiCacheFile(filePath);
        _backend.emojiRegistry().addCustomEmoji(name, filePath);
        _inFlightNames.remove(name);
        return;
    }

    NetworkRequest request(QStringLiteral("emoji/") + id + QStringLiteral("/image"));
    request.setPriority(QNetworkRequest::LowPriority);
    request.setAttribute(QNetworkRequest::BackgroundRequestAttribute, true);

    _httpConnector.get(request, HttpResponseCallback(
        [this, name, filePath](QVariant status, QByteArray data) {
            if (status.toInt() != QNetworkReply::NoError || data.isEmpty()
                || data.size() > MaxCustomEmojiDiskCacheBytes) {
                _inFlightNames.remove(name);
                return;
            }

            QFile file(filePath);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                _inFlightNames.remove(name);
                return;
            }
            if (file.write(data) != data.size()) {
                file.close();
                QFile::remove(filePath);
                _inFlightNames.remove(name);
                return;
            }
            file.close();

            pruneCustomEmojiDiskCache();
            if (QFileInfo::exists(filePath)) {
                _backend.emojiRegistry().addCustomEmoji(name, filePath);
            }
            _inFlightNames.remove(name);
        }));
}

} // namespace Mattermost
