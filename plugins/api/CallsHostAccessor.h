#pragma once

#include <QtPlugin>

namespace MatterLeast::PluginApi {

class CallsHostInterface;

// Optional capability injected before PluginInterface::enable(). The host
// outlives a loaded provider; disable() must stop using it before unload.
class CallsHostAccessor
{
public:
    virtual ~CallsHostAccessor() = default;
    virtual void setCallsHost(CallsHostInterface* host) = 0;
};

} // namespace MatterLeast::PluginApi

#define MATTERLEAST_CALLS_HOST_ACCESSOR_IID \
    "io.github.Ri0n.MatterLeast.CallsHostAccessor/1.0"
Q_DECLARE_INTERFACE(MatterLeast::PluginApi::CallsHostAccessor,
                    MATTERLEAST_CALLS_HOST_ACCESSOR_IID)
