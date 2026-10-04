#include <QObject>

#include "plugins/api/ConversationAccessor.h"
#include "plugins/api/ConversationAccessingHost.h"
#include "plugins/api/ConversationMenuInterface.h"
#include "plugins/api/PluginInterface.h"

class TestConversationPlugin final
    : public QObject
    , public MatterLeast::PluginApi::PluginInterface
    , public MatterLeast::PluginApi::ConversationAccessor
    , public MatterLeast::PluginApi::ConversationMenuInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MATTERLEAST_PLUGIN_IID FILE "TestConversationPlugin.json")
    Q_INTERFACES(MatterLeast::PluginApi::PluginInterface)
    Q_INTERFACES(MatterLeast::PluginApi::ConversationAccessor)
    Q_INTERFACES(MatterLeast::PluginApi::ConversationMenuInterface)

public:
    bool enable() override
    {
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
};

#include "TestConversationPlugin.moc"
