#include "build-config.h"

#if BUILD_MULTIMEDIA

#include "AttachmentMediaOpener.h"

#include <algorithm>
#include <utility>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

#include <qitemedia.h>

#include "backend/AttachmentService.h"
#include "backend/Backend.h"
#include "backend/types/BackendFile.h"

namespace Mattermost {

struct AttachmentMediaOpener::Waiter {
    QPointer<QObject> receiver;
    OpenDone done;
    bool cancelled = false;
};

struct AttachmentMediaOpener::Entry {
    QString fileId;
    QString path;
    ITEMediaState* state = nullptr;
    QPointer<QNetworkReply> reply;
    QVector<std::shared_ptr<Waiter>> waiters;
};

AttachmentMediaOpener& AttachmentMediaOpener::instance(Backend& backend)
{
    constexpr auto name = "attachmentMediaOpener";
    if (auto* existing = backend.findChild<AttachmentMediaOpener*>(
            QString::fromLatin1(name), Qt::FindDirectChildrenOnly)) {
        return *existing;
    }
    auto* opener = new AttachmentMediaOpener(backend);
    opener->setObjectName(QString::fromLatin1(name));
    return *opener;
}

AttachmentMediaOpener::AttachmentMediaOpener(Backend& backend)
    : ITEMediaOpener(&backend)
    , _backend(backend)
{
}

QUrl AttachmentMediaOpener::sourceFor(const BackendFile& file)
{
    QUrl url;
    url.setScheme(QStringLiteral("matterleast-media"));
    url.setPath(QLatin1Char('/') + file.id);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("name"), file.name);
    url.setQuery(query);
    return url;
}

QString AttachmentMediaOpener::cachedFilePath(const QUrl& source) const
{
    const auto entry = _entries.value(source.toString(QUrl::FullyEncoded));
    return entry && QFileInfo::exists(entry->path) ? entry->path : QString();
}

std::shared_ptr<AttachmentMediaOpener::Entry>
AttachmentMediaOpener::ensureEntry(const QUrl& source)
{
    if (source.isLocalFile()) {
        // qite retains the resolved file URL for replay/retry. Only recognize
        // files we ourselves placed in this backend's private media cache.
        const QString path = source.toLocalFile();
        for (const auto& entry : _entries) {
            if (entry->path == path
                || AttachmentService::instance(_backend).localFilePath(
                    entry->fileId, -1, path) == path)
                return entry;
        }
        return {};
    }
    if (source.scheme() != QLatin1String("matterleast-media")
        || source.path().size() < 2 || !_directory.isValid()) {
        return {};
    }
    const QString key = source.toString(QUrl::FullyEncoded);
    if (auto existing = _entries.value(key)) {
        return existing;
    }

    const QString name = QUrlQuery(source).queryItemValue(QStringLiteral("name"));
    QString extension = QFileInfo(name).suffix().toLower();
    static const QRegularExpression safeExtension(QStringLiteral("^[a-z0-9]{1,10}$"));
    if (!safeExtension.match(extension).hasMatch()) {
        extension.clear();
    }
    const QByteArray digest = QCryptographicHash::hash(
        key.toUtf8(), QCryptographicHash::Sha256).toHex();
    QString filename = QString::fromLatin1(digest);
    if (!extension.isEmpty()) {
        filename += QLatin1Char('.') + extension;
    }

    auto entry = std::make_shared<Entry>();
    entry->fileId = source.path().mid(1);
    entry->path = QDir(_directory.path()).filePath(filename);
    entry->state = new ITEMediaState(this);
    if (QFileInfo::exists(entry->path)
        || !AttachmentService::instance(_backend).localFilePath(entry->fileId).isEmpty()) {
        entry->state->setCachedRanges({{0, 1}});
    }
    _entries.insert(key, entry);
    return entry;
}

QIODevice* AttachmentMediaOpener::open(QUrl& source)
{
    const auto entry = ensureEntry(source);
    if (!entry) return nullptr;
    const QString saved = AttachmentService::instance(_backend)
        .localFilePath(entry->fileId, -1,
                       source.isLocalFile() ? source.toLocalFile() : QString());
    const QString path = QFileInfo::exists(entry->path) ? entry->path : saved;
    if (path.isEmpty()) return nullptr;
    auto* device = new QFile(path, this);
    if (!device->open(QIODevice::ReadOnly)) {
        delete device;
        return nullptr;
    }
    // Use a local URL with the correct extension as the decoder's format hint.
    // Authentication and network access are entirely owned by AttachmentService.
    source = QUrl::fromLocalFile(path);
    return device;
}

ITEMediaOpener::CancelOpen AttachmentMediaOpener::openAsync(
    QUrl source, QObject* receiver, OpenDone done)
{
    const auto entry = ensureEntry(source);
    if (!entry || !receiver) {
        if (receiver) {
            QTimer::singleShot(0, receiver, [source, done = std::move(done)] {
                if (done) done(nullptr, source, QStringLiteral("Media source unavailable"));
            });
        }
        return {};
    }

    const QString key = source.toString(QUrl::FullyEncoded);
    const auto waiter = std::make_shared<Waiter>();
    waiter->receiver = receiver;
    waiter->done = std::move(done);
    const QPointer<AttachmentMediaOpener> guard(this);
    const std::weak_ptr<Waiter> weak(waiter);
    const CancelOpen cancel = [guard, key, weak] {
        if (auto pending = weak.lock()) pending->cancelled = true;
        if (guard) guard->cancelUnused(key);
    };

    // A file already downloaded for this session is reusable across players.
    // Completion is queued so qite can establish its cancellation callback.
    if (QFileInfo::exists(entry->path)
        || !AttachmentService::instance(_backend).localFilePath(entry->fileId).isEmpty()) {
        entry->state->setCachedRanges({{0, 1}});
        QTimer::singleShot(0, this, [guard, waiter, source] {
            if (!guard || waiter->cancelled || !waiter->receiver) return;
            QUrl resolved = source;
            auto* device = guard->open(resolved);
            auto done = std::move(waiter->done);
            if (done) done(device, resolved,
                           device ? QString() : QStringLiteral("Cannot open media file"));
        });
        return cancel;
    }

    entry->waiters.push_back(waiter);
    connect(receiver, &QObject::destroyed, this, [guard, key, weak] {
        if (auto pending = weak.lock()) pending->cancelled = true;
        if (guard) guard->cancelUnused(key);
    });
    if (entry->reply) return cancel;

    entry->state->setSourceError({});
    entry->state->setDownload(true);
    const QString fileId = source.path().mid(1);
    entry->reply = AttachmentService::instance(_backend).downloadToFile(
        fileId, entry->path,
        [guard, key](qint64 received, qint64 total) {
            if (!guard) return;
            const auto current = guard->_entries.value(key);
            if (current && total > 0) {
                current->state->setDownload(
                    true, qBound(qreal(0), qreal(received) / total, qreal(1)));
            }
        },
        [guard, key, source](const QString& error) {
            if (!guard) return;
            const auto current = guard->_entries.value(key);
            if (!current) return;
            current->reply = nullptr;
            current->state->setDownload(false, -1, error);
            if (error.isEmpty()) {
                current->state->setCachedRanges({{0, 1}});
            }
            auto waiters = std::move(current->waiters);
            current->waiters.clear();
            for (const auto& pending : waiters) {
                if (pending->cancelled || !pending->receiver) continue;
                QUrl resolved = source;
                QIODevice* device = error.isEmpty() ? guard->open(resolved) : nullptr;
                auto done = std::move(pending->done);
                if (done) {
                    done(device, resolved, !error.isEmpty() ? error
                        : device ? QString() : QStringLiteral("Cannot open media file"));
                }
            }
        });
    return cancel;
}

void AttachmentMediaOpener::cancelUnused(const QString& key)
{
    const auto entry = _entries.value(key);
    if (!entry || !entry->reply) return;
    const bool hasWaiters = std::any_of(
        entry->waiters.cbegin(), entry->waiters.cend(),
        [](const std::shared_ptr<Waiter>& w) {
            return !w->cancelled && w->receiver;
        });
    if (!hasWaiters) {
        entry->reply->abort();
    }
}

void AttachmentMediaOpener::close(QIODevice* device)
{
    delete device;
}

void AttachmentMediaOpener::abort(QIODevice* device)
{
    // Local QFile reads are non-blocking and have nothing to interrupt.
    Q_UNUSED(device);
}

QVariant AttachmentMediaOpener::metadata(const QUrl& url)
{
    Q_UNUSED(url);
    return {};
}

ITEMediaState* AttachmentMediaOpener::mediaState(const QUrl& source)
{
    const auto entry = ensureEntry(source);
    return entry ? entry->state : nullptr;
}

void AttachmentMediaOpener::retry(const QUrl& source)
{
    if (const auto entry = ensureEntry(source)) {
        entry->state->setSourceError({});
        entry->state->setDownload(false);
    }
}

} // namespace Mattermost

#endif // BUILD_MULTIMEDIA
