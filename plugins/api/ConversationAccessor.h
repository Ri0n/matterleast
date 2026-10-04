#pragma once

#include <QtPlugin>

namespace MatterLeast::PluginApi {

class ConversationAccessingHost;

class ConversationAccessor
{
public:
    virtual ~ConversationAccessor() = default;

    virtual void setConversationAccessingHost(ConversationAccessingHost* host) = 0;
};

} // namespace MatterLeast::PluginApi

#define MATTERLEAST_CONVERSATION_ACCESSOR_IID \
    "io.github.Ri0n.MatterLeast.ConversationAccessor/1.0"
Q_DECLARE_INTERFACE(MatterLeast::PluginApi::ConversationAccessor,
                    MATTERLEAST_CONVERSATION_ACCESSOR_IID)
