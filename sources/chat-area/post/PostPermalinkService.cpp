#include "PostPermalinkService.h"
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QPointer>
#include <utility>
#include "PostForwardPolicy.h"
#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
namespace Mattermost {
PostPermalinkService& PostPermalinkService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<PostPermalinkService>> instances;
    QPointer<PostPermalinkService>& service = instances[&backend];
    if (!service) service = new PostPermalinkService(backend);
    return *service;
}
PostPermalinkService::PostPermalinkService(Backend& backend)
    : QObject(&backend)
    , backend_(backend)
    , connector_(&backend.networkSession())
{
}
QString PostPermalinkService::teamSlugForPost(Backend& backend, const BackendPost& post)
{
    Storage& storage = backend.getStorage();
    const BackendChannel* channel = storage.getChannelById(post.channel_id);
    const BackendTeam* team = channel ? channel->team : nullptr;
    if (!team) team = storage.getTeamById(backend.getCurrentTeamContextId());
    if (!team && !storage.teams.empty()) team = &storage.teams.begin()->second;
    return team ? team->name : QString();
}
QString PostPermalinkService::redirectPermalink(const Backend& backend, const QString& postId)
{
    if (postId.isEmpty()) {
        return {};
    }
    QString host = backend.networkSession().host();
    if (host.isEmpty()) {
        return {};
    }
    if (!host.endsWith(QLatin1Char('/'))) {
        host += QLatin1Char('/');
    }
    return host + QStringLiteral("_redirect/pl/") + postId;
}

void PostPermalinkService::resolve(const QString& postId,
                                   const QString& teamSlug,
                                   bool requirePreview,
                                   Callback callback)
{
    if (!callback) return;
    if (postId.isEmpty() || teamSlug.isEmpty()) {
        callback({}, tr("No team context is available for this message."));
        return;
    }
    // The server's configured SiteURL, NOT the local connection DNS alias,
    // determines whether Mattermost expands a forwarded post as a preview.
    NetworkRequest request(backend_.networkSession(), QStringLiteral("config/client?format=old"));
    const QPointer<PostPermalinkService> guard(this);
    connector_.get(request, HttpResponseCallback(
        [guard, postId, teamSlug, requirePreview,
         callback = std::move(callback)](QVariant status,
                                          const QJsonDocument& document) {
        if (!guard) return;
        if (status.toInt() != QNetworkReply::NoError || !document.isObject()) {
            callback({}, PostPermalinkService::tr("Cannot retrieve the server's canonical Site URL."));
            return;
        }
        const QJsonObject config = document.object();
        const QJsonValue enabled = config.value(QStringLiteral("EnablePermalinkPreviews"));
        if (requirePreview
            && (enabled.toString().compare(QLatin1String("false"), Qt::CaseInsensitive) == 0
                || (enabled.isBool() && !enabled.toBool()))) {
            callback({}, PostPermalinkService::tr("Permalink previews are disabled on this server."));
            return;
        }
        const QString link = nativeForwardPermalink(
            config.value(QStringLiteral("SiteURL")).toString(), teamSlug, postId);
        if (link.isEmpty()) {
            callback({}, PostPermalinkService::tr("The server did not provide a valid canonical message URL."));
            return;
        }
        callback(link, {});
    }));
}
} // namespace Mattermost
