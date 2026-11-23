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
#include <QTemporaryDir>
#include <QWidget>

#include "PluginHost.h"
#include "PluginSignature.h"
#include "backend/Backend.h"
#include "backend/Account.h"
#include "backend/AccountManager.h"
#include "backend/types/BackendUser.h"
#include "backend/types/BackendPost.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "navigation/AppNavigationService.h"
#include "navigation/ConversationReference.h"
#include "options/MLOptions.h"
#include "plugins/api/CallsHostInterface.h"
#include "plugins/api/CallsProviderInterface.h"
#include "plugins/api/ConversationAccessingHost.h"
#include "plugins/api/ConversationMenuInterface.h"
#include "plugins/api/MainWindowOverlayInterface.h"
#include "plugins/api/PluginSettingsInterface.h"

namespace Mattermost {
namespace {

const QString ManagerObjectName = QStringLiteral("matterleastPluginManager");
const char PluginActionProperty[] = "matterleastPluginAction";
const char PluginIdProperty[] = "matterleastPluginId";

bool nativePluginLoadingDisabled()
{
#ifdef MATTERLEAST_NATIVE_PLUGINS_DISABLED
    return true; // Enforced by the executable's build configuration.
#else
    const QCoreApplication* app = QCoreApplication::instance();
    return qEnvironmentVariable("MATTERLEAST_DISABLE_NATIVE_PLUGINS") == QLatin1String("1")
        || (app && app->arguments().contains(QStringLiteral("--disable-native-plugins")));
#endif
}

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
    explicit ConversationHost(AccountManager& accounts)
        : accounts_(accounts) {}

    QString displayName(const QString& conversationId) const override
    {
        Backend* backend = accounts_.activeBackend();
        if (!backend) return {};
        const BackendChannel* channel =
            backend->getStorage().getChannelById(conversationId);
        if (!channel) return {};
        const QString display = channel->display_name.trimmed();
        return display.isEmpty() ? channel->name.trimmed() : display;
    }

    QString reference(const QString& conversationId) const override
    {
        Backend* backend = accounts_.activeBackend();
        if (!backend) return {};
        BackendChannel* channel =
            backend->getStorage().getChannelById(conversationId);
        return channel ? ConversationReference::copyText(*backend, *channel)
                       : QString();
    }

    void open(const QString& conversationId,
              const QString& threadRootId) override
    {
        Backend* backend = accounts_.activeBackend();
        if (!backend) return;
        auto& navigation = AppNavigationService::instance(*backend);
        if (threadRootId.isEmpty()) navigation.openChannel(conversationId);
        else navigation.openThread(conversationId, threadRootId);
    }

    void openInNewTab(const QString& conversationId,
                      const QString& threadRootId) override
    {
        Backend* backend = accounts_.activeBackend();
        if (!backend) return;
        auto& navigation = AppNavigationService::instance(*backend);
        if (threadRootId.isEmpty()) navigation.openChannelInTab(conversationId);
        else navigation.openThreadInTab(conversationId, threadRootId);
    }

private:
    AccountManager& accounts_;
};

class PluginManager::CallsHost final : public MatterLeast::PluginApi::CallsHostInterface
{
public:
    explicit CallsHost(PluginManager& manager) : manager_(manager) {}

    bool authenticated(const QString& accountId) const override
    {
        const Backend* backend = manager_.backendForAccount(accountId);
        return backend && backend->loggedIn();
    }

    QString localUserId(const QString& accountId) const override
    {
        const Backend* backend = manager_.backendForAccount(accountId);
        return backend && backend->loggedIn() ? backend->getLoginUser().id : QString();
    }

    QString selectedAccountId() const override
    {
        return manager_.accounts_.selectedAccountId();
    }

    QString conversationDisplayName(const QString& accountId,
                                    const QString& channelId) const override
    {
        Backend* backend = manager_.backendForAccount(accountId);
        if (!backend || !backend->loggedIn()) return {};
        const BackendChannel* channel =
            backend->getStorage().getChannelById(channelId);
        if (!channel) return {};
        const QString display = channel->display_name.trimmed();
        return display.isEmpty() ? channel->name.trimmed() : display;
    }

    QString userDisplayName(const QString& accountId,
                            const QString& userId) const override
    {
        Backend* backend = manager_.backendForAccount(accountId);
        if (!backend || !backend->loggedIn()) return {};
        const BackendUser* user = backend->getStorage().getUserById(userId);
        return user ? user->getDisplayName() : QString();
    }

    bool sendSdp(const QString& accountId,
                 const QJsonObject& description) override
    {
        Backend* backend = manager_.backendForAccount(accountId);
        return backend && backend->loggedIn() && backend->sendCallsSdp(description);
    }

    bool sendAction(const QString& accountId, const QString& action,
                    const QJsonObject& data) override
    {
        Backend* backend = manager_.backendForAccount(accountId);
        return backend && backend->loggedIn()
            && backend->sendCallsPluginAction(action, data);
    }

    QString connectionId(const QString& accountId) const override
    {
        const Backend* backend = manager_.backendForAccount(accountId);
        return backend && backend->loggedIn() ? backend->callsConnectionId() : QString();
    }

    void request(const QString& accountId, const QString& method,
                 const QString& relativePath, const QJsonObject& body,
                 QObject* context, Reply completion) override
    {
        Backend* backend = manager_.backendForAccount(accountId);
        if (!backend || !backend->loggedIn()) {
            if (completion && context) {
                completion(0, QJsonObject{{QStringLiteral("error"),
                                           QStringLiteral("Account unavailable")}});
            }
            return;
        }
        backend->requestCallsPlugin(method, relativePath, body, context,
                                    std::move(completion));
    }

    void providerChanged() override
    {
        if (manager_.callsAvailable()) emit manager_.callsSnapshotChanged();
    }

private:
    PluginManager& manager_;
};

PluginManager& PluginManager::instance()
{
    auto* app = QCoreApplication::instance();
    auto* manager = app ? app->findChild<PluginManager*>(
        ManagerObjectName, Qt::FindDirectChildrenOnly) : nullptr;
    Q_ASSERT(manager); // The application owns one manager for every account.
    return *manager;
}

PluginManager::PluginManager(AccountManager& accounts, QObject* parent)
    : QObject(parent)
    , accounts_(accounts)
    , conversationHost_(std::make_unique<ConversationHost>(accounts))
    , callsHost_(std::make_unique<CallsHost>(*this))
{
    setObjectName(ManagerObjectName);
    connect(&accounts_, &AccountManager::accountRegistered, this,
            [this](Backend* backend) {
        if (backend) connectBackendSignals(*backend);
    });
    connect(&accounts_, &AccountManager::accountUnregistered, this,
            [this](Backend* backend) {
        const QString id = backend ? accountId(*backend) : QString();
        if (id.isEmpty()) return;
        if (callsSnapshot().accountId == id) leaveCall(id);
        clearIncomingCalls(id);
    });
    for (Backend* backend : accounts_.registeredBackends()) {
        connectBackendSignals(*backend);
    }
    connect(this, &PluginManager::callsSnapshotChanged,
            this, &PluginManager::handleIncomingCallSnapshot);
    connect(this, &PluginManager::callsAvailabilityChanged, this, [this](bool available) {
        if (!available) clearIncomingCalls();
    });
    if (QCoreApplication* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit,
                this, [this] { shutdown(); });
    }
}

void PluginManager::initializePlugins()
{
    loadStandardPlugins();
}

void PluginManager::restoreBackendConnections(Backend& backend)
{
    auto* app = QCoreApplication::instance();
    auto* manager = app ? app->findChild<PluginManager*>(
        ManagerObjectName, Qt::FindDirectChildrenOnly) : nullptr;
    if (manager && !manager->accountId(backend).isEmpty()) {
        manager->connectBackendSignals(backend);
    }
}

QString PluginManager::accountId(const Backend& backend) const
{
    const Account* account = accounts_.accountForBackend(&backend);
    return account ? account->id() : QString();
}

Backend* PluginManager::backendForAccount(const QString& id) const
{
    const Account* account = accounts_.account(id);
    return account ? account->backend() : nullptr;
}

void PluginManager::connectBackendSignals(Backend& backend)
{
    const QString id = accountId(backend);
    if (id.isEmpty()) return;

    connect(&backend, &Backend::onCallsPluginEvent, this,
            [this, id, owner = &backend](const QString& name, const QJsonObject& data,
                                        const QJsonObject& broadcast) {
        if (backendForAccount(id) != owner || !owner->loggedIn()) return;
        if (callsAvailable()) handleIncomingCallEvent(id, name, data, broadcast);
        if (auto* provider = callsProvider()) {
            provider->onServerEvent(id, name, data, broadcast);
        }
    });
    connect(&backend, &Backend::onNewPost, this,
            [this, id, owner = &backend](BackendChannel& channel, const BackendPost& post) {
        if (backendForAccount(id) != owner) return;
        if (post.type == QLatin1String("custom_calls")) {
            announceIncomingCall(id, channel.id);
        }
    });
    connect(&backend, &Backend::callPostEnded, this,
            [this, id, owner = &backend](const QString& channelId, const QString& postId) {
        if (backendForAccount(id) != owner) return;
        const auto invitation = incomingCalls_.constFind(incomingKey(id, channelId));
        if (invitation != incomingCalls_.cend() && invitation->call.postId == postId) {
            endIncomingCall(id, channelId);
        }
    });
    connect(&backend, &Backend::onWebSocketConnect, this, [this, id, owner = &backend] {
        if (backendForAccount(id) != owner) return;
        if (auto* provider = callsProvider()) provider->onConnectionChanged(id, true);
    });
    connect(&backend, &Backend::onWebSocketDisconnect, this, [this, id, owner = &backend] {
        if (backendForAccount(id) != owner) return;
        clearIncomingCalls(id);
        if (auto* provider = callsProvider()) provider->onConnectionChanged(id, false);
    });
    connect(&backend, &Backend::sessionEnding, this, [this, id, owner = &backend] {
        if (backendForAccount(id) != owner) return;
        leaveCall(id);
        clearIncomingCalls(id);
    });
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
    const QDir applicationDir(QCoreApplication::applicationDirPath());

#ifdef MATTERLEAST_SYSTEM_PLUGIN_RELATIVE_DIR
    // Installed Linux plugins live in libdir, not alongside /usr/bin.
    // Keep the adjacent directory only for uninstalled developer builds.
#ifdef MATTERLEAST_BUILD_EXECUTABLE_DIR
    if (QDir::cleanPath(applicationDir.absolutePath())
        == QDir::cleanPath(QString::fromLatin1(MATTERLEAST_BUILD_EXECUTABLE_DIR))) {
        paths.push_back(applicationDir.filePath(QStringLiteral("plugins")));
    }
#endif
    paths.push_back(QDir::cleanPath(applicationDir.filePath(
        QString::fromLatin1(MATTERLEAST_SYSTEM_PLUGIN_RELATIVE_DIR))));
#else
    paths.push_back(applicationDir.filePath(QStringLiteral("plugins")));
#endif

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
    for (const auto& existing : _mainWindows) {
        if (existing == &window) return;
    }
    _mainWindows.append(&window);
    for (const auto& host : plugins_) {
        if (host && host->isEnabled()) attachWindowOverlay(*host);
    }
}

void PluginManager::attachWindowOverlay(PluginHost& host)
{
    if (!host.isEnabled()) return;
    QObject* object = host.instance();
    auto* overlay = object
        ? qobject_cast<MatterLeast::PluginApi::MainWindowOverlayInterface*>(object)
        : nullptr;
    if (!overlay) return;
    for (const auto& window : _mainWindows) {
        if (window) overlay->attachToMainWindow(window);
    }
}

void PluginManager::loadStandardPlugins()
{
    // No metadata parsing or QPluginLoader::instance() for untrusted libraries.
    if (nativePluginLoadingDisabled()) {
        return;
    }
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
    if (nativePluginLoadingDisabled()) {
        return false;
    }
    QString loadPath = filePath;
#ifdef MATTERLEAST_TRUSTED_PLUGINS_ONLY
    if (!_verifiedPluginDirectory) {
        _verifiedPluginDirectory = std::make_unique<QTemporaryDir>();
    }
    loadPath = trustedPluginPath(filePath, _verifiedPluginDirectory.get());
    if (loadPath.isEmpty()) {
        qWarning() << "Rejected unsigned or untrusted native plugin:"
                   << QFileInfo(filePath).fileName();
        return false;
    }
#endif
    auto host = std::make_unique<PluginHost>(
        loadPath, *conversationHost_, callsHost_.get());
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

    const bool wasAvailable = callsAvailable();
    plugins_.push_back(std::move(host));
    if (callsAvailable() != wasAvailable) {
        emit callsAvailabilityChanged(callsAvailable());
        emit callsSnapshotChanged();
    }
    return true;
}

MatterLeast::PluginApi::CallsProviderInterface* PluginManager::callsProvider() const
{
    for (const auto& host : plugins_) {
        QObject* object = host && host->isEnabled() ? host->instance() : nullptr;
        auto* provider = object
            ? qobject_cast<MatterLeast::PluginApi::CallsProviderInterface*>(object)
            : nullptr;
        if (provider) {
            return provider;
        }
    }
    return nullptr;
}

bool PluginManager::callsAvailable() const
{
    return callsProvider() != nullptr;
}

MatterLeast::PluginApi::CallsSnapshot PluginManager::callsSnapshot() const
{
    if (auto* provider = callsProvider()) {
        return provider->snapshot();
    }
    return {};
}

bool PluginManager::startCall(const QString& accountId, const QString& channelId)
{
    auto* provider = callsProvider();
    return provider && backendForAccount(accountId) && !channelId.isEmpty()
        && provider->startCall(accountId, channelId);
}

bool PluginManager::joinCall(const QString& accountId, const QString& channelId)
{
    auto* provider = callsProvider();
    return provider && backendForAccount(accountId) && !channelId.isEmpty()
        && provider->joinCall(accountId, channelId);
}

bool PluginManager::leaveCall(const QString& accountId)
{
    auto* provider = callsProvider();
    return provider && provider->snapshot().accountId == accountId
        && provider->leaveCall();
}

bool PluginManager::setCallMicrophoneMuted(bool muted)
{
    auto* provider = callsProvider();
    return provider && provider->setMicrophoneMuted(muted);
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
    const bool wasAvailable = callsAvailable();
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
    if (callsAvailable() != wasAvailable) {
        emit callsAvailabilityChanged(callsAvailable());
    }
    emit callsSnapshotChanged();
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
