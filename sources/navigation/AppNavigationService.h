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
    void openUrlInTab(const QUrl& url);
    void openChannel(const QString& channelId);
    void openChannelInTab(const QString& channelId);
    void openPost(const QString& postId);
    void openPostInTab(const QString& postId);
    void openThread(const QString& channelId, const QString& rootId);
    void openThreadInTab(const QString& channelId, const QString& rootId);
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
    void tabRequested(const QString& channelId,
                      const QString& rootId,
                      const QString& postId,
                      const QStringList& contextPostIds,
                      bool reachedOldest,
                      bool reachedNewest);

private:
    explicit AppNavigationService(Backend& backend);

    quint64 beginNavigation();
    void ensureMainWindowConnection();
    void openUrlImpl(const QUrl& url, bool inTab);
    void openPostImpl(const QString& postId, bool inTab);
    bool activateExistingDestination(const QString& channelId,
                                     const QString& rootId = QString(),
                                     bool restoreBookmark = true);
    BackendChannel* findChannel(const QString& teamName,
                                const QString& channelName) const;
    BackendChannel* findPostChannel(const QString& postId) const;
    void openPostInChannel(BackendChannel& channel, const QString& postId,
                           quint64 navigationGeneration, bool inTab);
    void presentPost(const QString& channelId,
                     const QString& postId,
                     const QString& rootId,
                     const QStringList& contextPostIds,
                     bool reachedOldest,
                     bool reachedNewest,
                     bool inTab);

    Backend& backend;
    NavigationRequestGate navigationRequests;
};

} // namespace Mattermost
