#pragma once

#include <QList>
#include <QString>
#include <QVariantHash>
#include <QtPlugin>

namespace MatterLeast::PluginApi {

/**
 * Adds actions to the conversation/thread header menu without exposing any
 * MatterLeast core/UI classes across the plugin ABI.
 *
 * Each returned QVariantHash represents one action. Supported keys:
 * - id (QString, required): stable plugin-local action identifier
 * - text (QString, required): visible label
 * - enabled (bool, optional, default true)
 * - checkable (bool, optional, default false)
 * - checked (bool, optional, default false)
 * - separatorBefore (bool, optional, default false)
 *
 * New optional keys may be added without changing this interface ABI.
 */
class ConversationMenuInterface
{
public:
    virtual ~ConversationMenuInterface() = default;

    virtual QList<QVariantHash> conversationMenuActions(
        const QString& conversationId,
        const QString& threadRootId) const = 0;

    virtual void conversationMenuActionTriggered(
        const QString& actionId,
        const QString& conversationId,
        const QString& threadRootId,
        bool checked) = 0;
};

} // namespace MatterLeast::PluginApi

#define MATTERLEAST_CONVERSATION_MENU_IID \
    "io.github.Ri0n.MatterLeast.ConversationMenu/1.0"
Q_DECLARE_INTERFACE(MatterLeast::PluginApi::ConversationMenuInterface,
                    MATTERLEAST_CONVERSATION_MENU_IID)
