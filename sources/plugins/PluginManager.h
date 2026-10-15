#pragma once

#include <memory>
#include <vector>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantHash>

class QAction;
class QWidget;

namespace Mattermost {

class Backend;
class PluginHost;

struct PluginInfo
{
    QString id;
    QString name;
    QString version;
    QString filePath;
    bool enabled = false;
};

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

    /** Attach enabled window-overlay extensions to this main window. */
    void attachMainWindow(QWidget& window);

    QList<PluginInfo> pluginInfos() const;
    bool setPluginEnabled(const QString& pluginId, bool enabled);
    /** Descriptors include the persisted value; only enabled plugins expose settings. */
    QList<QVariantHash> pluginSettings(const QString& pluginId) const;
    bool setPluginSetting(const QString& pluginId,
                          const QString& key, const QVariant& value);
    QStringList loadedPluginIds() const;
    QStringList pluginSearchPaths() const;

private:
    class ConversationHost;

    explicit PluginManager(Backend& backend);
    void attachWindowOverlay(PluginHost& host);
    void restorePluginSettings(const QString& pluginId, QObject& object);
    void loadStandardPlugins();
    bool loadPluginFile(const QString& filePath);
    void shutdown();

    Backend& backend_;
    QPointer<QWidget> _mainWindow;
    std::unique_ptr<ConversationHost> conversationHost_;
    std::vector<std::unique_ptr<PluginHost>> plugins_;
};

} // namespace Mattermost
