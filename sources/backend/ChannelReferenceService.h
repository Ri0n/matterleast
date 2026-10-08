#pragma once

#include <functional>

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;

// Resolve channel references that are valid links but absent from the
// currently resident sidebar. Requests are keyed by team+slug and coalesced.
class ChannelReferenceService : public QObject
{
public:
    using Callback = std::function<void(const QString& displayName)>;

    static ChannelReferenceService& instance(Backend& backend);
    void resolve(const QString& teamName, const QString& slug, Callback callback);

private:
    explicit ChannelReferenceService(Backend& backend);

    HTTPConnector _http;
    QHash<QString, QString> _resolved;
    QHash<QString, QVector<Callback>> _pending;
};

} // namespace Mattermost
