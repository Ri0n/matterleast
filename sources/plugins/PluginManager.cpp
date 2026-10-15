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
#include <QWidget>

#include "PluginHost.h"
#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "navigation/AppNavigationService.h"
#include "navigation/ConversationReference.h"
#include "options/MLOptions.h"
#include "plugins/api/ConversationAccessingHost.h"
#include "plugins/api/ConversationMenuInterface.h"
#include "plugins/api/MainWindowOverlayInterface.h"
#include "plugins/api/PluginSettingsInterface.h"

namespace Mattermost {
namespace {

const QString ManagerObjectName = QStringLiteral("matterleastPluginManager");
const char PluginActionProperty[] = "matterleastPluginAction";
const char PluginIdProperty[] = "matterleastPluginId";

QString pluginEnabledOption(const QString& pluginId)
{
    return QStringLiteral("plugins/") + pluginId + QStringLiteral("/enabled");
}

QString pluginSettingOption(const QString& pluginId, const QString& key)
{
    return QStringLiteral("plugins/") + pluginId + QStringLiteral("/settings/") + key;
}

QVariant normalizeStoredSetting(const QVariantHash& descriptor, const QVariant& stored)
{
    const QString type = descriptor.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("integer")) {
        bool ok = false;
        const int number = stored.toInt(&ok);
        return ok ? QVariant(number) : QVariant();
    }
    if (type == QLatin1String("boolean")) {
        if (stored.userType() == QMetaType::Bool) {
            return stored;
        }
        const QString text = stored.toString().trimmed().toLower();
        if (text == QLatin1String("true") || text == QLatin1String("1")) {
            return QVariant(true);
        }
        if (text == QLatin1String("false") || text == QLatin1String("0")) {
            return QVariant(false);
        }
    }
    return {};
}

bool validSettingValue(const QVariantHash& descriptor, const QVariant& value)
{
    const QString key = descriptor.value(QStringLiteral("key")).toString();
    if (key.isEmpty() || key.contains(QLatin1Char('/'))) {
        return false;
    }
    const QString type = descriptor.value(QStringLiteral("type")).toString();
    const QVariant fallback = descriptor.value(QStringLiteral("default"));
    if (type == QLatin1String("integer")) {
        if (value.userType() != QMetaType::Int ||
            fallback.userType() != QMetaType::Int) {
            return false;
        }
        const int minimum = descriptor.value(QStringLiteral("minimum")).toInt();
        const int maximum = descriptor.value(QStringLiteral("maximum")).toInt();
        return minimum <= maximum && value.toInt() >= minimum
            && value.toInt() <= maximum;
    }
    if (type == QLatin1String("boolean")) {
        return value.userType() == QMetaType::Bool
            && fallback.userType() == QMetaType::Bool;
    }
    return false;
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

void PluginManager::attachMainWindow(QWidget& window)
{
    _mainWindow = &window;
    for (const auto& host : plugins_) {
        if (host && host->isEnabled()) {
            attachWindowOverlay(*host);
        }
    }
}

void PluginManager::attachWindowOverlay(PluginHost& host)
{
    if (!_mainWindow || !host.isEnabled()) {
        return;
    }
    QObject* object = host.instance();
    auto* overlay = object
        ? qobject_cast<MatterLeast::PluginApi::MainWindowOverlayInterface*>(object)
        : nullptr;
    if (overlay) {
        overlay->attachToMainWindow(_mainWindow);
    }
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
    if (shouldEnable && !host->loadAndEnable(
            [this, &pluginId](QObject& object) {
                restorePluginSettings(pluginId, object);
            })) {
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

    const bool lifecycleOk = enabled
        ? host.loadAndEnable([this, &host](QObject& object) {
            restorePluginSettings(host.id(), object);
        })
        : host.disableAndUnload();
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
    if (enabled) {
        attachWindowOverlay(host);
    }
    return true;
}

void PluginManager::restorePluginSettings(const QString& pluginId, QObject& object)
{
    auto* settings = qobject_cast<
        MatterLeast::PluginApi::PluginSettingsInterface*>(&object);
    if (!settings) {
        return;
    }
    for (const QVariantHash& descriptor : settings->settingDescriptors()) {
        const QVariant fallback = descriptor.value(QStringLiteral("default"));
        if (!validSettingValue(descriptor, fallback)) {
            qWarning() << "Ignoring invalid plugin setting descriptor:" << pluginId;
            continue;
        }
        const QString key = descriptor.value(QStringLiteral("key")).toString();
        const QVariant stored = normalizeStoredSetting(descriptor,
            MLOptions::instance()->value(pluginSettingOption(pluginId, key),
                                         fallback));
        if (!validSettingValue(descriptor, stored) ||
            !settings->setSettingValue(key, stored)) {
            settings->setSettingValue(key, fallback);
        }
    }
}

QList<QVariantHash> PluginManager::pluginSettings(const QString& pluginId) const
{
    const auto it = std::find_if(plugins_.cbegin(), plugins_.cend(),
        [&pluginId](const std::unique_ptr<PluginHost>& host) {
            return host && host->id() == pluginId && host->isEnabled();
        });
    if (it == plugins_.cend()) {
        return {};
    }
    auto* extension = qobject_cast<MatterLeast::PluginApi::PluginSettingsInterface*>(
        (*it)->instance());
    if (!extension) {
        return {};
    }
    QList<QVariantHash> result;
    for (QVariantHash descriptor : extension->settingDescriptors()) {
        const QVariant fallback = descriptor.value(QStringLiteral("default"));
        if (!validSettingValue(descriptor, fallback)) {
            continue;
        }
        const QString key = descriptor.value(QStringLiteral("key")).toString();
        const QVariant saved = normalizeStoredSetting(descriptor,
            MLOptions::instance()->value(pluginSettingOption(pluginId, key),
                                         fallback));
        descriptor.insert(QStringLiteral("value"),
                          validSettingValue(descriptor, saved) ? saved : fallback);
        result.push_back(std::move(descriptor));
    }
    return result;
}

bool PluginManager::setPluginSetting(const QString& pluginId,
                                     const QString& key, const QVariant& value)
{
    const auto it = std::find_if(plugins_.begin(), plugins_.end(),
        [&pluginId](const std::unique_ptr<PluginHost>& host) {
            return host && host->id() == pluginId && host->isEnabled();
        });
    if (it == plugins_.end()) {
        return false;
    }
    auto* extension = qobject_cast<MatterLeast::PluginApi::PluginSettingsInterface*>(
        (*it)->instance());
    if (!extension) {
        return false;
    }
    for (const QVariantHash& descriptor : extension->settingDescriptors()) {
        if (descriptor.value(QStringLiteral("key")).toString() != key) {
            continue;
        }
        if (!validSettingValue(descriptor, value) ||
            !extension->setSettingValue(key, value)) {
            return false;
        }
        MLOptions::instance()->setValue(pluginSettingOption(pluginId, key), value);
        return true;
    }
    return false;
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
