#include <QObject>

#include "plugins/api/ConversationAccessor.h"
#include "plugins/api/ConversationAccessingHost.h"
#include "plugins/api/ConversationMenuInterface.h"
#include "plugins/api/PluginInterface.h"
#include "plugins/api/PluginSettingsInterface.h"

class TestConversationPlugin final
    : public QObject
    , public MatterLeast::PluginApi::PluginInterface
    , public MatterLeast::PluginApi::ConversationAccessor
    , public MatterLeast::PluginApi::ConversationMenuInterface
    , public MatterLeast::PluginApi::PluginSettingsInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MATTERLEAST_PLUGIN_IID FILE "TestConversationPlugin.json")
    Q_INTERFACES(MatterLeast::PluginApi::PluginInterface)
    Q_INTERFACES(MatterLeast::PluginApi::ConversationAccessor)
    Q_INTERFACES(MatterLeast::PluginApi::ConversationMenuInterface)
    Q_INTERFACES(MatterLeast::PluginApi::PluginSettingsInterface)

public:
    QList<QVariantHash> settingDescriptors() const override
    {
        return {{
            {QStringLiteral("key"), QStringLiteral("probe")},
            {QStringLiteral("title"), QStringLiteral("Probe value")},
            {QStringLiteral("type"), QStringLiteral("integer")},
            {QStringLiteral("default"), 10},
            {QStringLiteral("minimum"), 1},
            {QStringLiteral("maximum"), 100}
        }};
    }

    bool setSettingValue(const QString& key, const QVariant& value) override
    {
        if (key != QLatin1String("probe") || value.userType() != QMetaType::Int
            || value.toInt() < 1 || value.toInt() > 100) {
            return false;
        }
        _probe = value.toInt();
        return true;
    }

    bool enable() override
    {
        setProperty("probeAtEnable", _probe);
        enabled_ = host_ != nullptr;
        return enabled_;
    }

    bool disable() override
    {
        enabled_ = false;
        return true;
    }

    void setConversationAccessingHost(
        MatterLeast::PluginApi::ConversationAccessingHost* host) override
    {
        host_ = host;
    }

    QList<QVariantHash> conversationMenuActions(
        const QString& conversationId,
        const QString&) const override
    {
        if (!enabled_ || !host_) {
            return {};
        }

        QVariantHash action;
        action.insert(QStringLiteral("id"), QStringLiteral("probe.open"));
        action.insert(QStringLiteral("text"), QStringLiteral("Probe conversation"));
        action.insert(QStringLiteral("enabled"),
                      host_->reference(conversationId)
                          == QStringLiteral("ref:") + conversationId);
        return {action};
    }

    void conversationMenuActionTriggered(
        const QString& actionId,
        const QString& conversationId,
        const QString& threadRootId,
        bool) override
    {
        if (enabled_ && host_ && actionId == QStringLiteral("probe.open")) {
            host_->open(conversationId, threadRootId);
        }
    }

private:
    MatterLeast::PluginApi::ConversationAccessingHost* host_ = nullptr;
    bool enabled_ = false;
    int _probe = 10;
};

#include "TestConversationPlugin.moc"
