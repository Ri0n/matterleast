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
    using IdCallback = std::function<void(const QString& channelId)>;

    static ChannelReferenceService& instance(Backend& backend);
    void resolve(const QString& teamName, const QString& slug, Callback callback);
    // Used by semantic navigation when the referenced channel is not yet in
    // local storage. Shares the same lookup/cache as display-name resolution.
    void resolveId(const QString& teamName, const QString& slug,
                   IdCallback callback);

private:
    struct Details {
        QString id;
        QString displayName;
    };
    using DetailsCallback = std::function<void(const Details&)>;

    explicit ChannelReferenceService(Backend& backend);
    void resolveDetails(const QString& teamName, const QString& slug,
                        DetailsCallback callback);

    HTTPConnector _http;
    QHash<QString, Details> _resolved;
    QHash<QString, QVector<DetailsCallback>> _pending;
};

} // namespace Mattermost
