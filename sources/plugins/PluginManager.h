#pragma once

#include <memory>
#include <vector>

#include <QList>
#include <QObject>
#include <QStringList>

class QAction;

namespace Mattermost {

class Backend;
class PluginHost;

class PluginManager final : public QObject
{
    Q_OBJECT
public:
    static PluginManager& instance(Backend& backend);
    ~PluginManager() override;

    QList<QAction*> createConversationMenuActions(
        QObject* parent,
        const QString& conversationId,
        const QString& threadRootId);

    QStringList loadedPluginIds() const;
    QStringList pluginSearchPaths() const;

private:
    class ConversationHost;

    explicit PluginManager(Backend& backend);
    void loadStandardPlugins();
    bool loadPluginFile(const QString& filePath);
    void shutdown();

    Backend& backend_;
    std::unique_ptr<ConversationHost> conversationHost_;
    std::vector<std::unique_ptr<PluginHost>> plugins_;
};

} // namespace Mattermost
