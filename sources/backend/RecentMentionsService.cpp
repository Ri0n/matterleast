#include "RecentMentionsService.h"

#include <utility>

#include <QHash>
#include <QPointer>

#include "Backend.h"
#include "RecentMentionsPolicy.h"

namespace Mattermost {

RecentMentionsService& RecentMentionsService::instance(Backend& sourceBackend)
{
    static QHash<Backend*, QPointer<RecentMentionsService>> instances;
    QPointer<RecentMentionsService>& service = instances[&sourceBackend];
    if (!service) {
        service = new RecentMentionsService(sourceBackend);
    }
    return *service;
}

RecentMentionsService::RecentMentionsService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
}

void RecentMentionsService::loadPage(int page,
                                     int perPage,
                                     CollectionCallback callback)
{
    const QString terms = RecentMentionsPolicy::searchTerms(backend.getLoginUser());
    PostRepository::instance(backend).searchRecentMentions(
        terms, page, perPage, std::move(callback));
}

} // namespace Mattermost
