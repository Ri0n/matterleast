#pragma once

#include <QJsonObject>
#include <QString>
#include <QtPlugin>

#include "CallsTypes.h"

namespace MatterLeast::PluginApi {

// Optional plugin capability, independent of GStreamer, libnice and the
// MatterLeast Backend. This is a capability IID, not the base Plugin IID.
//
// All methods run on the host's Qt thread. The provider must not block this
// thread on network/media work. It reports asynchronous changes via
// CallsHostInterface::providerChanged(), and returns a snapshot by value.
//
// PluginInterface::disable() MUST refuse unloading while an active call or
// asynchronous operation still owns plugin objects or media callbacks.
class CallsProviderInterface
{
public:
    virtual ~CallsProviderInterface() = default;

    virtual CallsSnapshot snapshot() const = 0;
    virtual bool startCall(const QString& channelId) = 0;
    virtual bool joinCall(const QString& channelId) = 0;
    virtual bool leaveCall() = 0;
    virtual bool setMicrophoneMuted(bool muted) = 0;

    // Only custom_* Mattermost WebSocket events reach this method. The
    // provider filters Calls event names and owns all call-specific parsing.
    virtual void onServerEvent(const QString& eventName,
                               const QJsonObject& data,
                               const QJsonObject& broadcast) = 0;
    virtual void onConnectionChanged(bool connected) = 0;
};

} // namespace MatterLeast::PluginApi

#define MATTERLEAST_CALLS_PROVIDER_IID \
    "io.github.Ri0n.MatterLeast.CallsProvider/1.0"
Q_DECLARE_INTERFACE(MatterLeast::PluginApi::CallsProviderInterface,
                    MATTERLEAST_CALLS_PROVIDER_IID)
