#pragma once
#include <functional>
#include <QObject>
#include <QString>
#include "backend/HTTPConnector.h"
namespace Mattermost {
class Backend;
class BackendPost;
// Canonical Mattermost permalinks may differ from the connection hostname.
class PostPermalinkService final : public QObject {
public:
    using Callback = std::function<void(const QString& url, const QString& error)>;
    static PostPermalinkService& instance(Backend& backend);
    static QString teamSlugForPost(Backend& backend, const BackendPost& post);
    // Universal link for Copy message link: no team or config lookup required.
    static QString redirectPermalink(const QString& postId);
    void resolve(const QString& postId, const QString& teamSlug,
                 bool requirePreview, Callback callback);
private:
    explicit PostPermalinkService(Backend& backend);
    HTTPConnector connector_;
};
} // namespace Mattermost
