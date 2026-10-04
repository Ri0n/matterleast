#pragma once

#include <memory>

#include <QJsonObject>
#include <QObject>
#include <QPluginLoader>
#include <QString>

#include "plugins/api/ConversationAccessingHost.h"
#include "plugins/api/PluginInterface.h"

namespace Mattermost {

class PluginHost final
{
public:
    PluginHost(QString filePath,
               MatterLeast::PluginApi::ConversationAccessingHost& conversationHost);
    ~PluginHost();

    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;

    bool loadAndEnable();
    bool disableAndUnload();

    bool isEnabled() const { return enabled_; }
    bool hasValidMetadata() const { return validMetadata_; }
    const QString& id() const { return id_; }
    const QString& name() const { return name_; }
    const QString& version() const { return version_; }
    const QString& filePath() const { return filePath_; }
    QObject* instance() const { return pluginObject_; }

private:
    void readMetadata();
    void injectAccessors(QObject& object);

    QString filePath_;
    QPluginLoader loader_;
    MatterLeast::PluginApi::ConversationAccessingHost& conversationHost_;
    QObject* pluginObject_ = nullptr;
    MatterLeast::PluginApi::PluginInterface* plugin_ = nullptr;
    QString id_;
    QString name_;
    QString version_;
    bool validMetadata_ = false;
    bool enabled_ = false;
};

} // namespace Mattermost
