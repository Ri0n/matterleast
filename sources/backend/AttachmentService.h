#pragma once

#include <functional>

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QVector>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;

class AttachmentService : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(const QByteArray&)>;

    static AttachmentService& instance(Backend& backend);

    void retrieveFile(const QString& fileId, Callback callback);
    void retrievePreview(const QString& fileId, Callback callback);
    void retrieveThumbnail(const QString& fileId, Callback callback);

private:
    explicit AttachmentService(Backend& backend);
    void retrieve(const QString& requestPath, Callback callback);

    Backend& backend;
    HTTPConnector httpConnector;
    QHash<QString, QVector<Callback>> pendingCallbacks;
};

} // namespace Mattermost
