#include <QObject>

#include "plugins/api/CallsHostAccessor.h"
#include "plugins/api/CallsHostInterface.h"
#include "plugins/api/CallsProviderInterface.h"
#include "plugins/api/PluginInterface.h"

// Deliberately no GStreamer, QtMultimedia, WebSocket, or backend dependency.
// This plugin verifies only the public host/provider ABI and unload policy.
class TestCallsPlugin final
    : public QObject
    , public MatterLeast::PluginApi::PluginInterface
    , public MatterLeast::PluginApi::CallsHostAccessor
    , public MatterLeast::PluginApi::CallsProviderInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MATTERLEAST_PLUGIN_IID FILE "TestCallsPlugin.json")
    Q_INTERFACES(MatterLeast::PluginApi::PluginInterface)
    Q_INTERFACES(MatterLeast::PluginApi::CallsHostAccessor)
    Q_INTERFACES(MatterLeast::PluginApi::CallsProviderInterface)

public:
    void setCallsHost(MatterLeast::PluginApi::CallsHostInterface* host) override
    {
        host_ = host;
    }

    bool enable() override
    {
        enabled_ = host_ && host_->authenticated();
        return enabled_;
    }

    bool disable() override
    {
        // A provider must not allow QPluginLoader to unload live call code.
        if (snapshot_.state != MatterLeast::PluginApi::CallsState::Idle) {
            return false;
        }
        enabled_ = false;
        return true;
    }

    MatterLeast::PluginApi::CallsSnapshot snapshot() const override
    {
        return snapshot_;
    }

    bool startCall(const QString& channelId) override { return enter(channelId); }
    bool joinCall(const QString& channelId) override { return enter(channelId); }

    bool leaveCall() override
    {
        if (!enabled_ || snapshot_.state != MatterLeast::PluginApi::CallsState::Connected) {
            return false;
        }
        snapshot_ = {};
        host_->providerChanged();
        return true;
    }

    bool setMicrophoneMuted(bool muted) override
    {
        if (!enabled_ || snapshot_.state != MatterLeast::PluginApi::CallsState::Connected) {
            return false;
        }
        snapshot_.microphoneMuted = muted;
        host_->providerChanged();
        return true;
    }

    void onServerEvent(const QString& name, const QJsonObject& data,
                       const QJsonObject&) override
    {
        if (!enabled_ || name != QLatin1String("custom_calls_test")) {
            return;
        }
        MatterLeast::PluginApi::CallsParticipant participant;
        participant.userId = data.value(QStringLiteral("user_id")).toString();
        if (participant.userId.isEmpty()) {
            return;
        }
        snapshot_.participants.append(participant);
        host_->providerChanged();
    }

    void onConnectionChanged(bool connected) override
    {
        if (enabled_ && !connected &&
            snapshot_.state == MatterLeast::PluginApi::CallsState::Connected) {
            snapshot_.state = MatterLeast::PluginApi::CallsState::Failed;
            snapshot_.errorMessage = QStringLiteral("Disconnected");
            host_->providerChanged();
        }
    }

private:
    bool enter(const QString& channelId)
    {
        if (!enabled_ || channelId.isEmpty() ||
            snapshot_.state != MatterLeast::PluginApi::CallsState::Idle) {
            return false;
        }
        snapshot_.channelId = channelId;
        snapshot_.callId = QStringLiteral("fake-call");
        snapshot_.state = MatterLeast::PluginApi::CallsState::Connected;
        host_->providerChanged();
        return true;
    }

    MatterLeast::PluginApi::CallsHostInterface* host_ = nullptr;
    MatterLeast::PluginApi::CallsSnapshot snapshot_;
    bool enabled_ = false;
};

#include "TestCallsPlugin.moc"
