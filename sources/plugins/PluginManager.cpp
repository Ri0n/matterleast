#include "PluginManager.h"

#include <algorithm>

#include <QAction>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QPointer>
#include <QStandardPaths>

#include "PluginHost.h"
#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "navigation/AppNavigationService.h"
#include "navigation/ConversationReference.h"
#include "options/MLOptions.h"
#include "plugins/api/ConversationAccessingHost.h"
#include "plugins/api/ConversationMenuInterface.h"

namespace Mattermost {
namespace {

const QString ManagerObjectName = QStringLiteral("matterleastPluginManager");
const char PluginActionProperty[] = "matterleastPluginAction";
const char PluginIdProperty[] = "matterleastPluginId";

QString pluginEnabledOption(const QString& pluginId)
{
    return QStringLiteral("plugins/") + pluginId + QStringLiteral("/enabled");
}

} // namespace

class PluginManager::ConversationHost final
    : public MatterLeast::PluginApi::ConversationAccessingHost
{
public:
    explicit ConversationHost(Backend& backend)
        : backend_(backend)
    {
    }

    QString displayName(const QString& conversationId) const override
    {
        const BackendChannel* channel =
            backend_.getStorage().getChannelById(conversationId);
        if (!channel) {
            return {};
        }
        const QString display = channel->display_name.trimmed();
        if (!display.isEmpty()) {
            return display;
        }
        return channel->name.trimmed();
    }

    QString reference(const QString& conversationId) const override
    {
        BackendChannel* channel =
            backend_.getStorage().getChannelById(conversationId);
        return channel ? ConversationReference::copyText(backend_, *channel)
                       : QString();
    }

    void open(const QString& conversationId,
              const QString& threadRootId) override
    {
        auto& navigation = AppNavigationService::instance(backend_);
        if (threadRootId.isEmpty()) {
            navigation.openChannel(conversationId);
        } else {
            navigation.openThread(conversationId, threadRootId);
        }
    }

    void openInNewTab(const QString& conversationId,
                      const QString& threadRootId) override
    {
        auto& navigation = AppNavigationService::instance(backend_);
        if (threadRootId.isEmpty()) {
            navigation.openChannelInTab(conversationId);
        } else {
            navigation.openThreadInTab(conversationId, threadRootId);
        }
    }

private:
    Backend& backend_;
};

PluginManager& PluginManager::instance(Backend& backend)
{
    if (auto* existing = backend.findChild<PluginManager*>(
            ManagerObjectName, Qt::FindDirectChildrenOnly)) {
        return *existing;
    }

    auto* manager = new PluginManager(backend);
    manager->setObjectName(ManagerObjectName);
    return *manager;
}

PluginManager::PluginManager(Backend& backend)
    : QObject(&backend)
    , backend_(backend)
    , conversationHost_(std::make_unique<ConversationHost>(backend))
{
    if (QCoreApplication* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit,
                this, [this] { shutdown(); });
    }
    loadStandardPlugins();
}

PluginManager::~PluginManager()
{
    shutdown();
}

void PluginManager::shutdown()
{
    while (!plugins_.empty()) {
        plugins_.pop_back();
    }
}

QStringList PluginManager::pluginSearchPaths() const
{
    QStringList paths;
    paths.push_back(QDir(QCoreApplication::applicationDirPath())
                        .filePath(QStringLiteral("plugins")));

    const QString userData =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!userData.isEmpty()) {
        const QString userPlugins =
            QDir(userData).filePath(QStringLiteral("plugins"));
        if (!paths.contains(userPlugins)) {
            paths.push_back(userPlugins);
        }
    }
    return paths;
}

void PluginManager::loadStandardPlugins()
{
    for (const QString& path : pluginSearchPaths()) {
        const QDir dir(path);
        if (!dir.exists()) {
            continue;
        }

        const QFileInfoList entries = dir.entryInfoList(
            QDir::Files | QDir::NoSymLinks, QDir::Name | QDir::IgnoreCase);
        for (const QFileInfo& entry : entries) {
            if (QLibrary::isLibrary(entry.absoluteFilePath())) {
                loadPluginFile(entry.absoluteFilePath());
            }
        }
    }
}

bool PluginManager::loadPluginFile(const QString& filePath)
{
    auto host = std::make_unique<PluginHost>(filePath, *conversationHost_);
    if (!host->hasValidMetadata()) {
        return false;
    }

    const QString pluginId = host->id();
    const auto duplicate = std::find_if(
        plugins_.cbegin(), plugins_.cend(),
        [&pluginId](const std::unique_ptr<PluginHost>& loaded) {
            return loaded && loaded->id() == pluginId;
        });
    if (duplicate != plugins_.cend()) {
        qWarning() << "Ignoring duplicate plugin id" << pluginId << "from" << filePath;
        return false;
    }

    const bool shouldEnable = MLOptions::instance()->value<bool>(
        pluginEnabledOption(pluginId), true);
    if (shouldEnable && !host->loadAndEnable()) {
        qWarning() << "Discovered MatterLeast plugin but could not enable"
                   << pluginId << "from" << filePath;
    } else if (host->isEnabled()) {
        qInfo() << "Loaded MatterLeast plugin" << host->id()
                << host->version() << "from" << filePath;
    }

    plugins_.push_back(std::move(host));
    return true;
}

QList<PluginInfo> PluginManager::pluginInfos() const
{
    QList<PluginInfo> infos;
    infos.reserve(static_cast<int>(plugins_.size()));
    for (const auto& host : plugins_) {
        if (!host) {
            continue;
        }
        PluginInfo info;
        info.id = host->id();
        info.name = host->name();
        info.version = host->version();
        info.filePath = host->filePath();
        info.enabled = host->isEnabled();
        infos.push_back(std::move(info));
    }
    return infos;
}

bool PluginManager::setPluginEnabled(const QString& pluginId, bool enabled)
{
    const auto it = std::find_if(
        plugins_.begin(), plugins_.end(),
        [&pluginId](const std::unique_ptr<PluginHost>& host) {
            return host && host->id() == pluginId;
        });
    if (it == plugins_.end()) {
        return false;
    }

    PluginHost& host = **it;
    if (host.isEnabled() == enabled) {
        MLOptions::instance()->setValue<bool>(pluginEnabledOption(pluginId), enabled);
        return true;
    }

    const bool lifecycleOk = enabled ? host.loadAndEnable() : host.disableAndUnload();
    if (!lifecycleOk && host.isEnabled() != enabled) {
        return false;
    }
    if (!lifecycleOk) {
        // disable() has already succeeded, but the operating system may keep
        // the library resident (for example because some external reference is
        // still alive). The plugin is nevertheless logically disabled, so keep
        // the UI and persisted preference aligned with the actual host state.
        qWarning() << "Plugin disabled but its library could not be unloaded:"
                   << pluginId;
    }

    MLOptions::instance()->setValue<bool>(pluginEnabledOption(pluginId), enabled);
    return true;
}

QStringList PluginManager::loadedPluginIds() const
{
    QStringList ids;
    for (const auto& host : plugins_) {
        if (host && host->isEnabled()) {
            ids.push_back(host->id());
        }
    }
    return ids;
}

QList<QAction*> PluginManager::createConversationMenuActions(
    QObject* parent,
    const QString& conversationId,
    const QString& threadRootId)
{
    QList<QAction*> result;

    for (const auto& host : plugins_) {
        QObject* object = host && host->isEnabled() ? host->instance() : nullptr;
        auto* extension = object
            ? qobject_cast<MatterLeast::PluginApi::ConversationMenuInterface*>(object)
            : nullptr;
        if (!extension) {
            continue;
        }

        const QList<QVariantHash> descriptors =
            extension->conversationMenuActions(conversationId, threadRootId);
        for (const QVariantHash& descriptor : descriptors) {
            const QString actionId =
                descriptor.value(QStringLiteral("id")).toString().trimmed();
            const QString text =
                descriptor.value(QStringLiteral("text")).toString().trimmed();
            if (actionId.isEmpty() || text.isEmpty()) {
                qWarning() << "Ignoring invalid conversation action from plugin"
                           << host->id();
                continue;
            }

            if (descriptor.value(QStringLiteral("separatorBefore"), false).toBool()
                && !result.isEmpty() && !result.back()->isSeparator()) {
                auto* separator = new QAction(parent);
                separator->setSeparator(true);
                separator->setProperty(PluginActionProperty, true);
                separator->setProperty(PluginIdProperty, host->id());
                result.push_back(separator);
            }

            auto* action = new QAction(text, parent);
            action->setProperty(PluginActionProperty, true);
            action->setProperty(PluginIdProperty, host->id());
            action->setEnabled(
                descriptor.value(QStringLiteral("enabled"), true).toBool());
            action->setCheckable(
                descriptor.value(QStringLiteral("checkable"), false).toBool());
            if (action->isCheckable()) {
                action->setChecked(
                    descriptor.value(QStringLiteral("checked"), false).toBool());
            }

            const QPointer<PluginManager> manager(this);
            const QString pluginId = host->id();
            connect(action, &QAction::triggered, action,
                    [manager, pluginId, actionId, conversationId,
                     threadRootId](bool checked) {
                if (!manager) {
                    return;
                }

                const auto currentHost = std::find_if(
                    manager->plugins_.cbegin(), manager->plugins_.cend(),
                    [&pluginId](const std::unique_ptr<PluginHost>& candidate) {
                        return candidate && candidate->id() == pluginId
                            && candidate->isEnabled();
                    });
                if (currentHost == manager->plugins_.cend()) {
                    return;
                }

                QObject* currentObject = (*currentHost)->instance();
                auto* current = currentObject
                    ? qobject_cast<
                        MatterLeast::PluginApi::ConversationMenuInterface*>(
                            currentObject)
                    : nullptr;
                if (!current) {
                    return;
                }
                current->conversationMenuActionTriggered(
                    actionId, conversationId, threadRootId, checked);
            });
            result.push_back(action);
        }
    }

    return result;
}

} // namespace Mattermost
