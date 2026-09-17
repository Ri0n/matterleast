#pragma once

#include <cstdint>
#include <functional>

#include <QObject>
#include <QStringList>
#include <QUrl>

#include "NavigationRequestGate.h"

namespace Mattermost {

class Backend;
class BackendChannel;

/** Semantic application navigation. Post retrieval belongs to PostRepository. */
class AppNavigationService final : public QObject
{
    Q_OBJECT
public:
    using NavigationCallback = std::function<void(bool)>;

    static AppNavigationService& instance(Backend& backend);

    void openUrl(const QUrl& url);
    void openChannel(const QString& channelId);
    void openPost(const QString& postId);
    void openThread(const QString& channelId, const QString& rootId);
    void openThreadAtLastViewed(const QString& channelId,
                                const QString& rootId,
                                uint64_t lastViewedAt,
                                const QString& fallbackPostId = QString(),
                                NavigationCallback callback = {},
                                bool preserveIfOpen = true);

signals:
    /** Emitted synchronously whenever a newer semantic navigation supersedes pending work. */
    void navigationStarted();

    void channelRequested(const QString& channelId,
                          const QString& postId,
                          const QString& rootId,
                          const QStringList& contextPostIds,
                          bool reachedOldest,
                          bool reachedNewest,
                          bool preserveIfOpen);

private:
    explicit AppNavigationService(Backend& backend);

    quint64 beginNavigation();
    void ensureMainWindowConnection();
    bool isLocalUrl(const QUrl& url) const;
    BackendChannel* findChannel(const QString& teamName,
                                const QString& channelName) const;
    BackendChannel* findPostChannel(const QString& postId) const;
    void openPostInChannel(BackendChannel& channel, const QString& postId,
                           quint64 navigationGeneration);

    Backend& backend;
    NavigationRequestGate navigationRequests;
};

} // namespace Mattermost
