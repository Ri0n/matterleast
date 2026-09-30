#pragma once

#include <functional>

#include <QObject>

#include "PostRepository.h"

namespace Mattermost {

class Backend;

/**
 * Builds the logged-in user's Recent Mentions query and delegates post REST
 * retrieval to PostRepository, preserving the repository as the single owner
 * of post search transport and collection normalization.
 */
class RecentMentionsService final : public QObject
{
    Q_OBJECT
public:
    using CollectionCallback = PostRepository::CollectionCallback;

    static RecentMentionsService& instance(Backend& backend);

    void loadPage(int page, int perPage, CollectionCallback callback);

private:
    explicit RecentMentionsService(Backend& backend);

    Backend& backend;
};

} // namespace Mattermost
