#pragma once

#include <QString>
#include <QtPlugin>

namespace MatterLeast::PluginApi {

class ConversationAccessingHost
{
public:
    virtual ~ConversationAccessingHost() = default;

    virtual QString displayName(const QString& conversationId) const = 0;
    virtual QString reference(const QString& conversationId) const = 0;
    virtual void open(const QString& conversationId,
                      const QString& threadRootId) = 0;
    virtual void openInNewTab(const QString& conversationId,
                              const QString& threadRootId) = 0;
};

} // namespace MatterLeast::PluginApi

#define MATTERLEAST_CONVERSATION_ACCESSING_HOST_IID \
    "io.github.Ri0n.MatterLeast.ConversationAccessingHost/1.0"
Q_DECLARE_INTERFACE(MatterLeast::PluginApi::ConversationAccessingHost,
                    MATTERLEAST_CONVERSATION_ACCESSING_HOST_IID)
