#include "PluginHost.h"

#include <utility>

#include <QDebug>
#include <QJsonObject>
#include <QLibrary>

#include "plugins/api/ConversationAccessor.h"
#include "plugins/api/PluginInterface.h"

namespace Mattermost {

PluginHost::PluginHost(
    QString filePath,
    MatterLeast::PluginApi::ConversationAccessingHost& conversationHost)
    : filePath_(std::move(filePath))
    , loader_(filePath_)
    , conversationHost_(conversationHost)
{
    // QPluginLoader sets PreventUnloadHint by default since Qt 5.7. MatterLeast
    // deliberately supports a real disable/unload lifecycle, so do not leave a
    // disabled plugin resident merely because of Qt's default load hint.
    loader_.setLoadHints(loader_.loadHints() & ~QLibrary::PreventUnloadHint);
    readMetadata();
}

PluginHost::~PluginHost()
{
    if (!disableAndUnload() && loader_.isLoaded()) {
        qWarning() << "Plugin refused clean shutdown:" << filePath_;
    }
}

void PluginHost::readMetadata()
{
    const QJsonObject root = loader_.metaData();
    const QJsonObject metadata = root.value(QStringLiteral("MetaData")).toObject();

    id_ = metadata.value(QStringLiteral("id")).toString().trimmed();
    name_ = metadata.value(QStringLiteral("name")).toString().trimmed();
    version_ = metadata.value(QStringLiteral("version")).toString().trimmed();

    validMetadata_ = !id_.isEmpty() && !name_.isEmpty();
}

void PluginHost::injectAccessors(QObject& object)
{
    if (auto* accessor = qobject_cast<MatterLeast::PluginApi::ConversationAccessor*>(&object)) {
        accessor->setConversationAccessingHost(&conversationHost_);
    }
}

bool PluginHost::loadAndEnable()
{
    if (enabled_) {
        return true;
    }
    if (!validMetadata_) {
        qWarning() << "Ignoring plugin with incomplete metadata:" << filePath_;
        return false;
    }

    QObject* object = loader_.instance();
    if (!object) {
        qWarning() << "Failed to load plugin" << filePath_ << ':' << loader_.errorString();
        return false;
    }

    auto* plugin = qobject_cast<MatterLeast::PluginApi::PluginInterface*>(object);
    if (!plugin) {
        qWarning() << "Library is not a MatterLeast plugin:" << filePath_;
        loader_.unload();
        return false;
    }

    injectAccessors(*object);
    if (!plugin->enable()) {
        qWarning() << "Plugin enable failed:" << id_;
        loader_.unload();
        return false;
    }

    pluginObject_ = object;
    plugin_ = plugin;
    enabled_ = true;
    return true;
}

bool PluginHost::disableAndUnload()
{
    if (enabled_) {
        if (!plugin_ || !plugin_->disable()) {
            return false;
        }
        enabled_ = false;
    }

    pluginObject_ = nullptr;
    plugin_ = nullptr;
    if (loader_.isLoaded()) {
        return loader_.unload();
    }
    return true;
}

} // namespace Mattermost
