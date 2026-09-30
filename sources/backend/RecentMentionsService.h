#pragma once

#include <functional>

#include <QObject>
#include <QString>

#include "HTTPConnector.h"
#include "PostRepository.h"

namespace Mattermost {

class Backend;
class BackendUser;

/**
 * Server-backed Recent Mentions collection matching Mattermost web semantics.
 *
 * This is a cross-conversation search projection, not a timeline source: it
 * searches all teams with OR semantics over the logged-in user's personal
 * mention keys and preserves the search endpoint's result order.
 */
class RecentMentionsService final : public QObject
{
    Q_OBJECT
public:
    using CollectionCallback = PostRepository::CollectionCallback;

    static RecentMentionsService& instance(Backend& backend);

    /** Mattermost personal mention terms, excluding broadcast mentions. */
    static QString mentionTerms(const BackendUser& user);

    void loadPage(int page, int perPage, CollectionCallback callback);

private:
    explicit RecentMentionsService(Backend& backend);

    Backend& backend;
    HTTPConnector httpConnector;
};

} // namespace Mattermost
