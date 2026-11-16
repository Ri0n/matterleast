#pragma once

#include "build-config.h"

#if BUILD_MULTIMEDIA

#include <memory>

#include <QHash>
#include <QPointer>
#include <QTemporaryDir>
#include <QUrl>
#include <QVector>

#include <qite.h>

namespace Mattermost {

class Backend;
class BackendFile;

// Backend-scoped source provider. A detached qite player may outlive the post
// and its ChatArea, so neither the media cache nor its source can belong to a
// message widget. The opener is destroyed when the backend/session closes.
class AttachmentMediaOpener final : public ITEMediaOpener
{
    Q_OBJECT
public:
    static AttachmentMediaOpener& instance(Backend& backend);
    static QUrl sourceFor(const BackendFile& file);

    QIODevice* open(QUrl& url) override;
    CancelOpen openAsync(QUrl url, QObject* receiver, OpenDone done) override;
    void close(QIODevice* device) override;
    void abort(QIODevice* device) override;
    QVariant metadata(const QUrl& url) override;
    ITEMediaState* mediaState(const QUrl& url) override;
    void retry(const QUrl& url) override;

private:
    struct Waiter;
    struct Entry;
    explicit AttachmentMediaOpener(Backend& backend);
    std::shared_ptr<Entry> ensureEntry(const QUrl& url);
    void cancelUnused(const QString& key);

    Backend& _backend;
    QTemporaryDir _directory;
    QHash<QString, std::shared_ptr<Entry>> _entries;
};

} // namespace Mattermost

#endif // BUILD_MULTIMEDIA
