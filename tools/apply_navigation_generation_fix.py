from pathlib import Path


def replace(path, old, new):
    p = Path(path)
    text = p.read_text()
    if old not in text:
        raise RuntimeError(f"pattern not found in {path}: {old[:120]!r}")
    p.write_text(text.replace(old, new, 1))


# Shared monotonic gate used to invalidate older asynchronous navigation work.
Path("sources/navigation/NavigationRequestGate.h").write_text(r'''#pragma once

#include <QtGlobal>

namespace Mattermost {

/**
 * Monotonic generation gate for semantic navigation.
 *
 * Every user navigation starts a new generation. Asynchronous callbacks keep
 * the generation they started with and may publish UI state only while it is
 * still current.
 */
class NavigationRequestGate
{
public:
    quint64 begin()
    {
        ++generation_;
        if (generation_ == 0) {
            ++generation_;
        }
        return generation_;
    }

    bool isCurrent(quint64 generation) const
    {
        return generation != 0 && generation == generation_;
    }

    quint64 current() const { return generation_; }

private:
    quint64 generation_ = 0;
};

} // namespace Mattermost
''')

# AppNavigationService: invalidate stale repository/network callbacks before they
# can publish an old target to MainWindow.
replace(
    "sources/navigation/AppNavigationService.h",
    '#include <QUrl>\n',
    '#include <QUrl>\n\n#include "NavigationRequestGate.h"\n')
replace(
    "sources/navigation/AppNavigationService.h",
    'signals:\n    void channelRequested(',
    'signals:\n    /** Emitted synchronously whenever a newer semantic navigation supersedes pending work. */\n    void navigationStarted();\n\n    void channelRequested(')
replace(
    "sources/navigation/AppNavigationService.h",
    '    void ensureMainWindowConnection();\n',
    '    quint64 beginNavigation();\n    void ensureMainWindowConnection();\n')
replace(
    "sources/navigation/AppNavigationService.h",
    '    void openPostInChannel(BackendChannel& channel, const QString& postId);\n\n    Backend& backend;\n',
    '    void openPostInChannel(BackendChannel& channel, const QString& postId,\n                           quint64 navigationGeneration);\n\n    Backend& backend;\n    NavigationRequestGate navigationRequests;\n')

replace(
    "sources/navigation/AppNavigationService.cpp",
    '''    ensureMainWindowConnection();\n}\n\nvoid AppNavigationService::ensureMainWindowConnection()\n''',
    '''    ensureMainWindowConnection();\n}\n\nquint64 AppNavigationService::beginNavigation()\n{\n    const quint64 generation = navigationRequests.begin();\n    emit navigationStarted();\n    return generation;\n}\n\nvoid AppNavigationService::ensureMainWindowConnection()\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''            connect(this, &AppNavigationService::channelRequested,\n                    mainWindow, &MainWindow::openChannelPost,\n                    Qt::UniqueConnection);\n''',
    '''            connect(this, &AppNavigationService::navigationStarted,\n                    mainWindow, &MainWindow::beginSemanticNavigation,\n                    Qt::UniqueConnection);\n            connect(this, &AppNavigationService::channelRequested,\n                    mainWindow, &MainWindow::openChannelPost,\n                    Qt::UniqueConnection);\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''void AppNavigationService::openChannel(const QString& channelId)\n{\n    if (!channelId.isEmpty() && backend.getStorage().getChannelById(channelId)) {\n''',
    '''void AppNavigationService::openChannel(const QString& channelId)\n{\n    beginNavigation();\n    if (!channelId.isEmpty() && backend.getStorage().getChannelById(channelId)) {\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''void AppNavigationService::openThread(const QString& channelId, const QString& rootId)\n{\n    if (channelId.isEmpty() || rootId.isEmpty()\n''',
    '''void AppNavigationService::openThread(const QString& channelId, const QString& rootId)\n{\n    beginNavigation();\n    if (channelId.isEmpty() || rootId.isEmpty()\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''    if (!isLocalUrl(url)) {\n        QDesktopServices::openUrl(url);\n        return;\n    }\n''',
    '''    if (!isLocalUrl(url)) {\n        beginNavigation();\n        QDesktopServices::openUrl(url);\n        return;\n    }\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''    const QUrl browserUrl = url.isRelative()\n        ? QUrl(NetworkRequest::host()).resolved(url)\n        : url;\n    QDesktopServices::openUrl(browserUrl);\n''',
    '''    const QUrl browserUrl = url.isRelative()\n        ? QUrl(NetworkRequest::host()).resolved(url)\n        : url;\n    beginNavigation();\n    QDesktopServices::openUrl(browserUrl);\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''void AppNavigationService::openPost(const QString& postId)\n{\n    if (postId.isEmpty()) {\n''',
    '''void AppNavigationService::openPost(const QString& postId)\n{\n    const quint64 navigationGeneration = beginNavigation();\n    if (postId.isEmpty()) {\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''    if (BackendChannel* channel = findPostChannel(postId)) {\n        openPostInChannel(*channel, postId);\n        return;\n    }\n''',
    '''    if (BackendChannel* channel = findPostChannel(postId)) {\n        openPostInChannel(*channel, postId, navigationGeneration);\n        return;\n    }\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''        [guard, postId](BackendChannel* channel) {\n            if (!guard) {\n                return;\n            }\n''',
    '''        [guard, postId, navigationGeneration](BackendChannel* channel) {\n            if (!guard || !guard->navigationRequests.isCurrent(navigationGeneration)) {\n                return;\n            }\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''            guard->openPostInChannel(*channel, postId);\n''',
    '''            guard->openPostInChannel(*channel, postId, navigationGeneration);\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''{\n    BackendChannel* channel = backend.getStorage().getChannelById(channelId);\n    if (!channel || rootId.isEmpty()) {\n''',
    '''{\n    const quint64 navigationGeneration = beginNavigation();\n    BackendChannel* channel = backend.getStorage().getChannelById(channelId);\n    if (!channel || rootId.isEmpty()) {\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''        [guard, channelId, rootId, lastViewedAt, fallbackPostId, preserveIfOpen,\n         callback = std::move(callback)](const PostRepository::Page& page) mutable {\n            if (!guard) {\n                return;\n            }\n''',
    '''        [guard, channelId, rootId, lastViewedAt, fallbackPostId, preserveIfOpen,\n         navigationGeneration, callback = std::move(callback)](const PostRepository::Page& page) mutable {\n            if (!guard || !guard->navigationRequests.isCurrent(navigationGeneration)) {\n                if (callback) {\n                    callback(false);\n                }\n                return;\n            }\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''void AppNavigationService::openPostInChannel(BackendChannel& channel,\n                                             const QString& postId)\n{\n''',
    '''void AppNavigationService::openPostInChannel(BackendChannel& channel,\n                                             const QString& postId,\n                                             quint64 navigationGeneration)\n{\n    if (!navigationRequests.isCurrent(navigationGeneration)) {\n        return;\n    }\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''            const auto presentReply = [this, channelId, postId, rootId] {\n                ensureMainWindowConnection();\n''',
    '''            const auto presentReply = [this, channelId, postId, rootId, navigationGeneration] {\n                if (!navigationRequests.isCurrent(navigationGeneration)) {\n                    return;\n                }\n                ensureMainWindowConnection();\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''                [guard, channelId, postId, rootId](const PostRepository::PostResult& result) {\n                    if (!guard || !result.success || result.channelId != channelId) {\n''',
    '''                [guard, channelId, postId, rootId, navigationGeneration](const PostRepository::PostResult& result) {\n                    if (!guard\n                        || !guard->navigationRequests.isCurrent(navigationGeneration)\n                        || !result.success || result.channelId != channelId) {\n''')
replace(
    "sources/navigation/AppNavigationService.cpp",
    '''        [guard, channelId, postId](const PostRepository::Context& context) {\n            if (!guard || !context.success) {\n''',
    '''        [guard, channelId, postId, navigationGeneration](const PostRepository::Context& context) {\n            if (!guard || !guard->navigationRequests.isCurrent(navigationGeneration)\n                || !context.success) {\n''')

# MainWindow: any new semantic navigation synchronously invalidates queued UI
# stages from the previous request, even when the new context is still loading.
replace(
    "sources/mainwindow.h",
    '''    void installRealtimeUiSync();\n\tvoid openChannelPost(''',
    '''    void installRealtimeUiSync();\n    void beginSemanticNavigation();\n\tvoid openChannelPost(''')
replace(
    "sources/mainwindow.h",
    '''\tQString\t\t\t\t\t\t\t\tretainedUnreadFilterChannelId;\n''',
    '''\tQString\t\t\t\t\t\t\t\tretainedUnreadFilterChannelId;\n    quint64                             semanticNavigationGeneration = 0;\n''')

replace(
    "sources/MainWindowNavigation.cpp",
    '''} // namespace\n\nvoid MainWindow::openChannelPost''',
    '''} // namespace\n\nvoid MainWindow::beginSemanticNavigation()\n{\n    ++semanticNavigationGeneration;\n    if (semanticNavigationGeneration == 0) {\n        ++semanticNavigationGeneration;\n    }\n}\n\nvoid MainWindow::openChannelPost''')
replace(
    "sources/MainWindowNavigation.cpp",
    '''{\n    if (channelId.isEmpty()) {\n''',
    '''{\n    const quint64 navigationGeneration = semanticNavigationGeneration;\n    if (channelId.isEmpty()) {\n''')
replace(
    "sources/MainWindowNavigation.cpp",
    '''    const auto armStoredChannelRetry = [this, channelId, postId, rootId,\n                                        contextPostIds, reachedOldest,\n                                        reachedNewest, preserveIfOpen] {\n''',
    '''    const auto armStoredChannelRetry = [this, channelId, postId, rootId,\n                                        contextPostIds, reachedOldest,\n                                        reachedNewest, preserveIfOpen,\n                                        navigationGeneration] {\n''')
replace(
    "sources/MainWindowNavigation.cpp",
    '''                              [guard, connection, channelId, postId, rootId,\n                               contextPostIds, reachedOldest, reachedNewest,\n                               preserveIfOpen](const QString& completedChannelId,\n''',
    '''                              [guard, connection, channelId, postId, rootId,\n                               contextPostIds, reachedOldest, reachedNewest,\n                               preserveIfOpen, navigationGeneration](const QString& completedChannelId,\n''')
replace(
    "sources/MainWindowNavigation.cpp",
    '''            QObject::disconnect(*connection);\n            if (!guard || !opened) {\n                return;\n            }\n\n            QTimer::singleShot(0, guard.data(),\n                [guard, channelId, postId, rootId, contextPostIds,\n                 reachedOldest, reachedNewest, preserveIfOpen] {\n                    if (guard) {\n''',
    '''            QObject::disconnect(*connection);\n            if (!guard || !opened\n                || navigationGeneration != guard->semanticNavigationGeneration) {\n                return;\n            }\n\n            QTimer::singleShot(0, guard.data(),\n                [guard, channelId, postId, rootId, contextPostIds,\n                 reachedOldest, reachedNewest, preserveIfOpen, navigationGeneration] {\n                    if (guard && navigationGeneration == guard->semanticNavigationGeneration) {\n''')
replace(
    "sources/MainWindowNavigation.cpp",
    '''            QTimer::singleShot(0, this,\n                [guard, channelId, postId, rootId, contextPostIds,\n                 reachedOldest, reachedNewest, preserveIfOpen] {\n                    if (guard) {\n''',
    '''            QTimer::singleShot(0, this,\n                [guard, channelId, postId, rootId, contextPostIds,\n                 reachedOldest, reachedNewest, preserveIfOpen, navigationGeneration] {\n                    if (guard && navigationGeneration == guard->semanticNavigationGeneration) {\n''')
replace(
    "sources/MainWindowNavigation.cpp",
    '''        QPointer<ChatArea> threadGuard(threadArea);\n        QTimer::singleShot(0, threadArea,\n            [threadGuard, postId] {\n                if (!threadGuard || !threadGuard->lockNavigationToPost(postId, 0)) {\n''',
    '''        QPointer<ChatArea> threadGuard(threadArea);\n        QPointer<MainWindow> windowGuard(this);\n        QTimer::singleShot(0, threadArea,\n            [threadGuard, windowGuard, postId, navigationGeneration] {\n                if (!threadGuard || !windowGuard\n                    || navigationGeneration != windowGuard->semanticNavigationGeneration\n                    || !threadGuard->lockNavigationToPost(postId, 0)) {\n''')
replace(
    "sources/MainWindowNavigation.cpp",
    '''    QPointer<ChatArea> areaGuard(area);\n    QTimer::singleShot(0, area,\n        [areaGuard, postId, contextPostIds, reachedOldest, reachedNewest] {\n            if (!areaGuard) {\n''',
    '''    QPointer<ChatArea> areaGuard(area);\n    QPointer<MainWindow> windowGuard(this);\n    QTimer::singleShot(0, area,\n        [areaGuard, windowGuard, postId, contextPostIds, reachedOldest, reachedNewest,\n         navigationGeneration] {\n            if (!areaGuard || !windowGuard\n                || navigationGeneration != windowGuard->semanticNavigationGeneration) {\n''')

# Regression test for the core latest-navigation-wins invariant.
Path("tests/NavigationRequestGateTest.cpp").write_text(r'''#include <QtTest>

#include "navigation/NavigationRequestGate.h"

using namespace Mattermost;

class NavigationRequestGateTest : public QObject
{
    Q_OBJECT

private slots:
    void newerRequestSupersedesOlderCallback()
    {
        NavigationRequestGate gate;
        const quint64 first = gate.begin();
        QVERIFY(gate.isCurrent(first));

        const quint64 second = gate.begin();
        QVERIFY(second > first);
        QVERIFY(!gate.isCurrent(first));
        QVERIFY(gate.isCurrent(second));
    }
};

QTEST_APPLESS_MAIN(NavigationRequestGateTest)
#include "NavigationRequestGateTest.moc"
''')

cmake = Path("tests/CMakeLists.txt")
text = cmake.read_text()
anchor = '''add_test(NAME post-navigation-window-test COMMAND post-navigation-window-test)\n'''
addition = anchor + r'''

add_executable(navigation-request-gate-test NavigationRequestGateTest.cpp)
target_include_directories(navigation-request-gate-test PRIVATE "${CMAKE_SOURCE_DIR}/sources")
target_link_libraries(navigation-request-gate-test PRIVATE Qt${QT_VERSION_MAJOR}::Test Qt${QT_VERSION_MAJOR}::Core)
add_test(NAME navigation-request-gate-test COMMAND navigation-request-gate-test)
'''
if anchor not in text:
    raise RuntimeError("CMake navigation test anchor not found")
cmake.write_text(text.replace(anchor, addition, 1))
