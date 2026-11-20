#include <memory>
#include <utility>

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QUrl>

#include "CallsWindow.h"
#include "GstAudioEngine.h"
#include "plugins/api/CallsHostAccessor.h"
#include "plugins/api/ConversationAccessor.h"
#include "plugins/api/ConversationAccessingHost.h"
#include "plugins/api/CallsHostInterface.h"
#include "plugins/api/CallsProviderInterface.h"
#include "plugins/api/ConversationMenuInterface.h"
#include "plugins/api/PluginInterface.h"

namespace {

constexpr auto CallsPrefix = "custom_com.mattermost.calls_";
constexpr auto CallsHttpRoot = "plugins/com.mattermost.calls/";

QStringList iceServerUrls(const QJsonObject& config)
{
    QStringList urls;
    auto appendUrl = [&urls](QString url, const QString& user,
                             const QString& password) {
        const bool turn = url.startsWith(QLatin1String("turn:"))
            || url.startsWith(QLatin1String("turns:"));
        if (turn || url.startsWith(QLatin1String("stun:"))) {
            if (!url.contains(QLatin1String("://"))) {
                const int colon = url.indexOf(QLatin1Char(':'));
                url.insert(colon + 1, QLatin1String("//"));
            }
            QUrl parsed(url);
            if (turn) {
                // GStreamer expects credentials in the TURN URI, whereas
                // browsers receive username/credential as separate fields.
                if (!user.isEmpty()) parsed.setUserName(user);
                if (!password.isEmpty()) parsed.setPassword(password);
            }
            if (!parsed.isValid() || parsed.host().isEmpty()) return;
            url = parsed.toString(QUrl::FullyEncoded);
        }
        urls.push_back(url);
    };

    for (const QJsonValue& entry : config.value(QStringLiteral("ICEServersConfigs")).toArray()) {
        const QJsonObject server = entry.toObject();
        const QString user = server.value(QStringLiteral("username")).toString();
        const QString password = server.value(QStringLiteral("credential")).toString();
        const QJsonValue value = server.value(QStringLiteral("urls"));
        if (value.isString()) {
            appendUrl(value.toString(), user, password);
        } else {
            for (const QJsonValue& url : value.toArray()) {
                if (url.isString()) appendUrl(url.toString(), user, password);
            }
        }
    }
    // Older servers also advertise ICEServers as a list without credentials.
    for (const QJsonValue& value : config.value(QStringLiteral("ICEServers")).toArray()) {
        if (value.isString()) appendUrl(value.toString(), {}, {});
    }
    urls.removeDuplicates();
    return urls;
}

} // namespace

class CallsGStreamerPlugin final
    : public QObject
    , public MatterLeast::PluginApi::PluginInterface
    , public MatterLeast::PluginApi::CallsHostAccessor
    , public MatterLeast::PluginApi::ConversationAccessor
    , public MatterLeast::PluginApi::CallsProviderInterface
    , public MatterLeast::PluginApi::ConversationMenuInterface
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID MATTERLEAST_PLUGIN_IID FILE "CallsPlugin.json")
    Q_INTERFACES(MatterLeast::PluginApi::PluginInterface)
    Q_INTERFACES(MatterLeast::PluginApi::CallsHostAccessor)
    Q_INTERFACES(MatterLeast::PluginApi::ConversationAccessor)
    Q_INTERFACES(MatterLeast::PluginApi::CallsProviderInterface)
    Q_INTERFACES(MatterLeast::PluginApi::ConversationMenuInterface)

public:
    void setCallsHost(MatterLeast::PluginApi::CallsHostInterface* host) override
    {
        host_ = host;
    }

    void setConversationAccessingHost(
        MatterLeast::PluginApi::ConversationAccessingHost* host) override
    {
        conversationHost_ = host;
    }

    bool enable() override
    {
        enabled_ = host_ != nullptr;
        return enabled_;
    }

    bool disable() override
    {
        // Never unload callbacks, pad handlers, or a GLib worker still
        // belonging to this native shared library.
        if (snapshot_.state != MatterLeast::PluginApi::CallsState::Idle
            || engine_) {
            return false;
        }
        window_.reset(); // Destroy Qt widgets before QPluginLoader unloads code.
        enabled_ = false;
        return true;
    }

    MatterLeast::PluginApi::CallsSnapshot snapshot() const override
    {
        return snapshot_;
    }

    bool startCall(const QString& channelId) override { return joinCall(channelId); }

    bool joinCall(const QString& channelId) override
    {
        using MatterLeast::PluginApi::CallsState;
        if (!enabled_ || !host_->authenticated() || host_->connectionId().isEmpty()
            || channelId.isEmpty() || snapshot_.state != CallsState::Idle) {
            return false;
        }

        const QString unavailable = GstAudioEngine::checkRuntime();
        if (!unavailable.isEmpty()) {
            // The optional package may be installed without a usable media
            // device or required GStreamer registry plugin. Report the
            // concrete failure in the same call window, not only in logs.
            qWarning() << "MatterLeast Calls unavailable:" << unavailable;
            snapshot_ = {};
            snapshot_.state = CallsState::Failed;
            snapshot_.channelId = channelId;
            snapshot_.errorMessage = unavailable;
            changed();
            showCallWindow();
            return false;
        }

        const quint64 generation = ++generation_;
        snapshot_ = {};
        snapshot_.state = CallsState::Connecting;
        snapshot_.channelId = channelId;
        originalConnectionId_ = host_->connectionId();
        changed();
        showCallWindow();

        // Official Calls configuration is served at /plugins/..., not at
        // /api/v4/. Fetch it before joining so ICE servers are available.
        host_->request(QStringLiteral("GET"),
                       QLatin1String(CallsHttpRoot) + QStringLiteral("config"),
                       {}, this,
                       [this, generation](int status, const QJsonObject& response) {
            if (generation != generation_ || !enabled_
                || snapshot_.state != CallsState::Connecting) {
                return; // User left or changed account while HTTP was pending.
            }
            if (status < 200 || status >= 300) {
                if (status == 404) {
                    fail(tr("Mattermost Calls API not found (HTTP 404). "
                            "Check that the Calls server plugin is enabled "
                            "and the /plugins/ path is forwarded to Mattermost."));
                } else if (status == 0) {
                    fail(tr("Could not request Mattermost Calls configuration: %1")
                             .arg(response.value(QStringLiteral("error")).toString()));
                } else {
                    fail(tr("Could not retrieve Mattermost Calls configuration "
                            "(HTTP %1)").arg(status));
                }
                return;
            }
            iceUrls_ = iceServerUrls(response);
            if (!host_->sendAction(QLatin1String(CallsPrefix) + QStringLiteral("join"),
                                   {{QStringLiteral("channelID"), snapshot_.channelId}})) {
                fail(QStringLiteral("Could not send Mattermost Calls join request"));
            }
        });
        return true;
    }

    bool leaveCall() override
    {
        using MatterLeast::PluginApi::CallsState;
        if (!enabled_ || snapshot_.state == CallsState::Idle) return false;

        ++generation_;
        snapshot_.state = CallsState::Leaving;
        changed();
        // The Calls WS protocol has no leave acknowledgement. Always clean
        // up locally, even when the backend connection is already offline.
        host_->sendAction(QLatin1String(CallsPrefix) + QStringLiteral("leave"), {});
        engine_.reset(); // Blocks until the GLib thread and all RTP pads stop.
        snapshot_ = {};
        iceUrls_.clear();
        originalConnectionId_.clear();
        changed();
        return true;
    }

    bool setMicrophoneMuted(bool muted) override
    {
        if (!engine_ ||
            snapshot_.state != MatterLeast::PluginApi::CallsState::Connected) {
            return false;
        }
        if (snapshot_.microphoneMuted == muted) return true;
        const QString action = muted ? QStringLiteral("mute")
                                     : QStringLiteral("unmute");
        if (!host_->sendAction(QLatin1String(CallsPrefix) + action, {})) {
            return false;
        }
        engine_->setMicrophoneMuted(muted);
        snapshot_.microphoneMuted = muted;
        for (auto& participant : snapshot_.participants) {
            if (participant.sessionId == originalConnectionId_) {
                participant.microphoneMuted = muted;
            }
        }
        changed();
        return true;
    }

    void onServerEvent(const QString& name, const QJsonObject& data,
                       const QJsonObject& broadcast) override
    {
        using MatterLeast::PluginApi::CallsState;
        if (!enabled_ || snapshot_.state == CallsState::Idle
            || !name.startsWith(QLatin1String(CallsPrefix))) {
            return;
        }
        const QString channel = data.value(QStringLiteral("channelID")).toString();
        const QString broadcastChannel =
            broadcast.value(QStringLiteral("channel_id")).toString();
        const QString eventConnection =
            data.value(QStringLiteral("connID")).toString();
        const bool isOurSession = !eventConnection.isEmpty()
            && (eventConnection == originalConnectionId_
                || eventConnection == host_->connectionId());
        const bool isOurChannel = channel == snapshot_.channelId
            || broadcastChannel == snapshot_.channelId
            || data.value(QStringLiteral("channel_id")).toString() == snapshot_.channelId;

        if (name == QLatin1String("custom_com.mattermost.calls_error")
            && isOurSession) {
            fail(data.value(QStringLiteral("data")).toString());
            return;
        }

        if (name == QLatin1String("custom_com.mattermost.calls_join")
            && isOurSession && snapshot_.state == CallsState::Connecting
            && !engine_) {
            beginMedia();
            return;
        }

        if (name == QLatin1String("custom_com.mattermost.calls_signal")
            && isOurSession && engine_) {
            const QByteArray message = data.value(QStringLiteral("data"))
                                           .toString().toUtf8();
            const QJsonObject decoded = QJsonDocument::fromJson(message).object();
            const QString type = decoded.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("offer") || type == QLatin1String("answer")) {
                engine_->receiveDescription(decoded);
            } else if (type == QLatin1String("candidate")) {
                const QJsonValue candidate = decoded.value(QStringLiteral("candidate"));
                if (candidate.isObject()) {
                    engine_->receiveCandidate(candidate.toObject());
                } else if (candidate.isString()) {
                    engine_->receiveCandidate({
                        {QStringLiteral("candidate"), candidate.toString()},
                        {QStringLiteral("sdpMLineIndex"),
                         decoded.value(QStringLiteral("sdpMLineIndex"))}
                    });
                }
            }
            return;
        }

        // The join response is followed by an authoritative Calls state
        // snapshot (call_state). A late joiner did not observe earlier
        // user_joined events, so only the snapshot can enumerate users who
        // were already in the conference before this client joined.
        // The field is JSON serialized inside the Mattermost WS data object.
        if (name == QLatin1String("custom_com.mattermost.calls_call_state")) {
            if (data.value(QStringLiteral("channel_id")).toString()
                != snapshot_.channelId) {
                return;
            }
            QJsonParseError parseError;
            const QJsonDocument parsed = QJsonDocument::fromJson(
                data.value(QStringLiteral("call")).toString().toUtf8(),
                &parseError);
            if (parseError.error != QJsonParseError::NoError || !parsed.isObject()) {
                return;
            }
            const QJsonObject call = parsed.object();
            const QString id = call.value(QStringLiteral("id")).toString();
            const QJsonValue sessions = call.value(QStringLiteral("sessions"));
            if (id.isEmpty() || !sessions.isArray()) {
                return;
            }

            QList<MatterLeast::PluginApi::CallsParticipant> participants;
            for (const QJsonValue& entry : sessions.toArray()) {
                const QJsonObject session = entry.toObject();
                MatterLeast::PluginApi::CallsParticipant participant;
                participant.userId = session.value(QStringLiteral("user_id")).toString();
                participant.sessionId = session.value(QStringLiteral("session_id")).toString();
                participant.microphoneMuted =
                    !session.value(QStringLiteral("unmuted")).toBool();
                if (!participant.userId.isEmpty() && !participant.sessionId.isEmpty()) {
                    participants.append(participant);
                }
            }
            snapshot_.callId = id;
            snapshot_.participants = std::move(participants);
            changed();
            return;
        }

        if (!isOurChannel) return;
        const QString sessionId = data.value(QStringLiteral("session_id")).toString();
        if (sessionId.isEmpty()) return;

        if (name == QLatin1String("custom_com.mattermost.calls_host_mute")) {
            // A host_mute is addressed to the affected user. Acknowledge with
            // our ordinary mute action, which broadcasts user_muted.
            if (sessionId == originalConnectionId_ && engine_) {
                if (snapshot_.state == CallsState::Connected) {
                    setMicrophoneMuted(true);
                } else {
                    engine_->setMicrophoneMuted(true);
                    snapshot_.microphoneMuted = true;
                    changed();
                }
            }
            return;
        }

        const bool mutedEvent =
            name == QLatin1String("custom_com.mattermost.calls_user_muted");
        const bool unmutedEvent =
            name == QLatin1String("custom_com.mattermost.calls_user_unmuted");
        if (mutedEvent || unmutedEvent) {
            // Official Calls events use userID, unlike user_joined (user_id).
            // Session ID is the stable key, even for multiple devices.
            const QString userId = data.value(QStringLiteral("userID")).toString();
            const bool muted = mutedEvent;
            for (auto& participant : snapshot_.participants) {
                if (participant.sessionId != sessionId
                    || (!userId.isEmpty() && participant.userId != userId)) {
                    continue;
                }
                if (participant.microphoneMuted != muted) {
                    participant.microphoneMuted = muted;
                    changed();
                }
                break;
            }
            if (sessionId == originalConnectionId_
                && snapshot_.microphoneMuted != muted && engine_) {
                snapshot_.microphoneMuted = muted;
                engine_->setMicrophoneMuted(muted);
                changed();
            }
            return;
        }

        const QString userId = data.value(QStringLiteral("user_id")).toString();
        if (userId.isEmpty()) return;
        if (name == QLatin1String("custom_com.mattermost.calls_user_joined")) {
            for (const auto& participant : snapshot_.participants) {
                if (participant.sessionId == sessionId) return;
            }
            MatterLeast::PluginApi::CallsParticipant participant;
            participant.userId = userId;
            participant.sessionId = sessionId;
            snapshot_.participants.append(participant);
            changed();
        } else if (name == QLatin1String("custom_com.mattermost.calls_user_left")) {
            for (int i = snapshot_.participants.size() - 1; i >= 0; --i) {
                if (snapshot_.participants[i].sessionId == sessionId) {
                    snapshot_.participants.removeAt(i);
                    changed();
                }
            }
        }
    }

    void onConnectionChanged(bool connected) override
    {
        using MatterLeast::PluginApi::CallsState;
        if (!enabled_ || snapshot_.state == CallsState::Idle) return;
        if (!connected) {
            // Preserve media/session state until the app's established
            // WebSocket attempts its normal reconnect.
            previousConnectionId_ = host_->connectionId();
            snapshot_.state = CallsState::Connecting;
            changed();
            return;
        }
        if (!originalConnectionId_.isEmpty()) {
            if (!host_->sendAction(QLatin1String(CallsPrefix)
                                  + QStringLiteral("reconnect"), {
                    {QStringLiteral("channelID"), snapshot_.channelId},
                    {QStringLiteral("originalConnID"), originalConnectionId_},
                    {QStringLiteral("prevConnID"), previousConnectionId_}
                })) {
                fail(QStringLiteral("Failed to resume Mattermost Calls session"));
            }
        }
    }

    QList<QVariantHash> conversationMenuActions(
        const QString& conversationId, const QString& threadRootId) const override
    {
        using MatterLeast::PluginApi::CallsState;
        if (!enabled_ || !host_->authenticated() || !threadRootId.isEmpty()) {
            return {};
        }
        const bool active = snapshot_.state != CallsState::Idle;
        if (active && conversationId != snapshot_.channelId) return {};
        return {{
            {QStringLiteral("id"), active ? QStringLiteral("calls.leave")
                                           : QStringLiteral("calls.join")},
            {QStringLiteral("text"), active ? tr("Leave audio call")
                                             : tr("Join or start audio call")},
            {QStringLiteral("enabled"), !active || snapshot_.state != CallsState::Leaving},
            {QStringLiteral("separatorBefore"), true}
        }};
    }

    void conversationMenuActionTriggered(
        const QString& actionId, const QString& conversationId,
        const QString&, bool) override
    {
        if (actionId == QLatin1String("calls.join")) joinCall(conversationId);
        if (actionId == QLatin1String("calls.leave")
            && conversationId == snapshot_.channelId) {
            leaveCall();
        }
    }

private:
    void showCallWindow()
    {
        if (!window_) {
            window_ = std::make_unique<CallsWindow>(
                [this](bool muted) { return setMicrophoneMuted(muted); },
                [this] { leaveCall(); },
                host_ ? host_->localUserId() : QString());
            window_->setMicrophoneDeviceHandlers(
                [this](const QString& id) {
                    if (engine_) engine_->setAudioInputDevice(id);
                },
                [this] {
                    if (engine_) engine_->requestDevices();
                });
        }
        QString title;
        if (conversationHost_) {
            title = conversationHost_->displayName(snapshot_.channelId);
        }
        window_->showForCall(snapshot_, title);
    }

    void beginMedia()
    {
        engine_ = std::make_unique<GstAudioEngine>(
            this,
            [this](const QJsonObject& description) {
                if (snapshot_.state == MatterLeast::PluginApi::CallsState::Connecting
                    || snapshot_.state == MatterLeast::PluginApi::CallsState::Connected) {
                    if (!host_->sendSdp(description)) {
                        fail(QStringLiteral("Failed to send Calls SDP"));
                    }
                }
            },
            [this](const QJsonObject& candidate) {
                if (snapshot_.state == MatterLeast::PluginApi::CallsState::Connecting
                    || snapshot_.state == MatterLeast::PluginApi::CallsState::Connected) {
                    host_->sendAction(QLatin1String(CallsPrefix) + QStringLiteral("ice"),
                        {{QStringLiteral("data"), QString::fromUtf8(
                            QJsonDocument(candidate).toJson(QJsonDocument::Compact))}});
                }
            },
            [this](const QString& error) { fail(error); },
            [this] {
                if (snapshot_.state == MatterLeast::PluginApi::CallsState::Connecting) {
                    snapshot_.state = MatterLeast::PluginApi::CallsState::Connected;
                    changed();
                    // The server initializes Unmuted=false. Our microphone
                    // starts active, so publish that initial state once media
                    // is actually connected to the SFU.
                    if (!snapshot_.microphoneMuted
                        && !host_->sendAction(QLatin1String(CallsPrefix)
                                              + QStringLiteral("unmute"), {})) {
                        fail(QStringLiteral("Failed to publish microphone state"));
                    }
                }
            },
            iceUrls_,
            [this](const QList<CallsMediaDevice>& devices,
                   const QString& selectedMicrophone) {
                if (window_) {
                    window_->setMicrophoneDevices(devices, selectedMicrophone);
                }
            });
        engine_->start();
        engine_->requestDevices();
    }

    void fail(const QString& error)
    {
        snapshot_.state = MatterLeast::PluginApi::CallsState::Failed;
        snapshot_.errorMessage = error;
        qWarning() << "MatterLeast Calls:" << error;
        changed();
    }

    void changed()
    {
        if (window_) window_->updateSnapshot(snapshot_);
        if (host_) host_->providerChanged();
    }

    MatterLeast::PluginApi::CallsHostInterface* host_ = nullptr;
    MatterLeast::PluginApi::ConversationAccessingHost* conversationHost_ = nullptr;
    MatterLeast::PluginApi::CallsSnapshot snapshot_;
    std::unique_ptr<CallsWindow> window_;
    std::unique_ptr<GstAudioEngine> engine_;
    QStringList iceUrls_;
    QString originalConnectionId_;
    QString previousConnectionId_;
    quint64 generation_ = 0;
    bool enabled_ = false;
};

#include "CallsPlugin.moc"
