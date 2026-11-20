#include "GstAudioEngine.h"

#include <atomic>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QDebug>
#include <QHash>
#include <QJsonValue>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QVariant>

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

namespace {

struct PendingIce {
    int mline = -1;
    QString candidate;
};

QString gstError(GError* error)
{
    return error ? QString::fromUtf8(error->message)
                 : QStringLiteral("Unknown GStreamer error");
}

} // namespace

struct GstAudioEngine::Impl
{
    Impl(QObject* receiver, DescriptionHandler onDescription,
         CandidateHandler onCandidate, ErrorHandler onError,
         ConnectedHandler onConnected, QStringList iceServers,
         DevicesHandler onDevices)
        : receiver_(receiver)
        , onDescription_(std::move(onDescription))
        , onCandidate_(std::move(onCandidate))
        , onError_(std::move(onError))
        , onConnected_(std::move(onConnected))
        , onDevices_(std::move(onDevices))
        , iceServers_(std::move(iceServers))
    {
        context_ = g_main_context_new();
        loop_ = g_main_loop_new(context_, FALSE);
    }

    ~Impl()
    {
        stop();
        if (loop_) g_main_loop_unref(loop_);
        if (context_) g_main_context_unref(context_);
    }

    void notifyError(const QString& reason)
    {
        if (!receiver_) return;
        QMetaObject::invokeMethod(receiver_, [cb = onError_, reason] {
            if (cb) cb(reason);
        }, Qt::QueuedConnection);
    }

    void notifyConnected()
    {
        if (!receiver_) return;
        QMetaObject::invokeMethod(receiver_, [cb = onConnected_] {
            if (cb) cb();
        }, Qt::QueuedConnection);
    }

    void notifyDescription(const QString& type, QString sdp)
    {
        // RTCD v1.2.6 compares MimeType against audio/opus case-sensitively.
        // GStreamer 1.24 emits OPUS. Normalize only on the signaling boundary;
        // keep GStreamer's local SDP/caps uppercase to avoid negotiation bugs.
        sdp.replace(QStringLiteral("OPUS/48000/2"),
                    QStringLiteral("opus/48000/2"));
        const QJsonObject description {
            {QStringLiteral("type"), type},
            {QStringLiteral("sdp"), sdp}
        };
        if (!receiver_) return;
        QMetaObject::invokeMethod(receiver_,
            [cb = onDescription_, description] {
                if (cb) cb(description);
            }, Qt::QueuedConnection);
    }

    void notifyCandidate(guint mline, const gchar* candidate)
    {
        if (!receiver_) return;
        const QJsonObject ice {
            {QStringLiteral("candidate"), QString::fromUtf8(candidate)},
            {QStringLiteral("sdpMLineIndex"), int(mline)}
        };
        QMetaObject::invokeMethod(receiver_,
            [cb = onCandidate_, ice] {
                if (cb) cb(ice);
            }, Qt::QueuedConnection);
    }

    void notifyDevices(const QList<CallsMediaDevice>& devices)
    {
        if (!receiver_ || !onDevices_) return;
        QMetaObject::invokeMethod(receiver_,
            [cb = onDevices_, devices, selected = selectedAudioId_] {
                if (cb) cb(devices, selected);
            }, Qt::QueuedConnection);
    }

    // Always enqueue on a GSource: g_main_context_invoke_full() may execute
    // synchronously on the caller's (Qt) thread when it acquires the context.
    void enqueue(std::function<void()> operation)
    {
        auto* fn = new std::function<void()>(std::move(operation));
        GSource* source = g_idle_source_new();
        g_source_set_callback(source, [](gpointer userData) -> gboolean {
            (*static_cast<std::function<void()>*>(userData))();
            return G_SOURCE_REMOVE;
        }, fn, [](gpointer userData) {
            delete static_cast<std::function<void()>*>(userData);
        });
        g_source_attach(source, context_);
        g_source_unref(source);
        g_main_context_wakeup(context_);
    }

    void post(std::function<void()> operation)
    {
        if (running_.load()) enqueue(std::move(operation));
    }

    void start()
    {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] {
            g_main_context_push_thread_default(context_);
            g_main_loop_run(loop_);
            g_main_context_pop_thread_default(context_);
        });
        enqueue([this] { buildPipeline(); });
    }

    void stop()
    {
        if (!running_.exchange(false)) return;
        enqueue([this] {
            if (pipeline_) {
                gst_element_set_state(pipeline_, GST_STATE_NULL);
                gst_object_unref(pipeline_);
                pipeline_ = nullptr;
                webrtc_ = nullptr;
                volume_ = nullptr;
                valve_ = nullptr;
                mixer_ = nullptr;
                audioInput_ = nullptr;
                audioConvert_ = nullptr;
            }
            for (auto it = deviceCache_.begin(); it != deviceCache_.end(); ++it) {
                gst_object_unref(it.value());
            }
            deviceCache_.clear();
            g_main_loop_quit(loop_);
        });
        if (thread_.joinable()) thread_.join();
    }

    static GstBusSyncReply onBus(GstBus*, GstMessage* message, gpointer userData)
    {
        auto* self = static_cast<Impl*>(userData);
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            self->notifyError(gstError(error));
            if (error) g_error_free(error);
            g_free(debug);
        }
        // No asynchronous bus watch is installed: retaining messages would
        // otherwise grow the pipeline bus queue for the entire call.
        return GST_BUS_DROP;
    }

    static void onNegotiationNeeded(GstElement*, gpointer userData)
    {
        auto* self = static_cast<Impl*>(userData);
        self->post([self] { self->makeOffer(); });
    }

    static void onIceCandidate(GstElement*, guint mline, gchar* candidate,
                               gpointer userData)
    {
        static_cast<Impl*>(userData)->notifyCandidate(mline, candidate);
    }

    static void onConnectionState(GObject* object, GParamSpec*, gpointer userData)
    {
        auto* self = static_cast<Impl*>(userData);
        GstWebRTCPeerConnectionState state = GST_WEBRTC_PEER_CONNECTION_STATE_NEW;
        g_object_get(object, "connection-state", &state, nullptr);
        if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED) {
            self->notifyConnected();
        }
    }

    static void onRemotePad(GstElement*, GstPad* pad, gpointer userData)
    {
        // This MUST be synchronous: deferring pad linking to the Qt/GLib
        // idle queue loses the first RTP packet and can produce NOT_LINKED.
        static_cast<Impl*>(userData)->attachRemoteAudio(pad);
    }

    void buildPipeline()
    {
        pipeline_ = gst_pipeline_new("matterleast-calls-audio");
        webrtc_ = gst_element_factory_make("webrtcbin", "calls-peer");
        mixer_ = gst_element_factory_make("audiomixer", "remote-mixer");
        volume_ = gst_element_factory_make("volume", "microphone-mute");
        valve_ = gst_element_factory_make("valve", "microphone-rtp-gate");
        const bool testAudio = qEnvironmentVariable("MATTERLEAST_CALLS_TEST_FAKE_AUDIO")
            == QLatin1String("1");
        GstElement* input = gst_element_factory_make(
            testAudio ? "audiotestsrc" : "autoaudiosrc", "microphone");
        GstElement* convert = gst_element_factory_make("audioconvert", nullptr);
        GstElement* resample = gst_element_factory_make("audioresample", nullptr);
        GstElement* encoder = gst_element_factory_make("opusenc", nullptr);
        GstElement* payloader = gst_element_factory_make("rtpopuspay", nullptr);
        GstElement* capsFilter = gst_element_factory_make("capsfilter", nullptr);
        GstElement* playbackConvert = gst_element_factory_make("audioconvert", nullptr);
        GstElement* playback = gst_element_factory_make(
            testAudio ? "fakesink" : "autoaudiosink", "speaker");
        std::vector<GstElement*> elements = {
            webrtc_, mixer_, volume_, valve_, input, convert, resample,
            encoder, payloader, capsFilter, playbackConvert, playback
        };

        for (auto* element : elements) {
            if (!element || !pipeline_) {
                notifyError(QStringLiteral("Missing GStreamer audio/WebRTC element"));
                for (auto* item : elements) {
                    if (item) gst_object_unref(item);
                }
                if (pipeline_) gst_object_unref(pipeline_);
                pipeline_ = nullptr;
                webrtc_ = nullptr;
                volume_ = nullptr;
                valve_ = nullptr;
                mixer_ = nullptr;
                return;
            }
        }

        g_object_set(webrtc_, "bundle-policy",
                     GST_WEBRTC_BUNDLE_POLICY_MAX_COMPAT, nullptr);
        if (testAudio) g_object_set(input, "is-live", TRUE, nullptr);
        g_object_set(playback, "sync", FALSE, nullptr);

        GstCaps* caps = gst_caps_from_string(
            "application/x-rtp,media=(string)audio,"
            "encoding-name=(string)OPUS,payload=(int)111,"
            "clock-rate=(int)48000,encoding-params=(string)2,"
            "sprop-stereo=(string)0,sprop-maxcapturerate=(string)48000");
        g_object_set(capsFilter, "caps", caps, nullptr);
        gst_caps_unref(caps);

        for (const QString& url : iceServers_) {
            const QByteArray encoded = url.toUtf8();
            if (url.startsWith(QStringLiteral("stun://"))) {
                g_object_set(webrtc_, "stun-server", encoded.constData(), nullptr);
            } else if (url.startsWith(QStringLiteral("turn://"))
                       || url.startsWith(QStringLiteral("turns://"))) {
                gboolean accepted = FALSE;
                g_signal_emit_by_name(webrtc_, "add-turn-server",
                                      encoded.constData(), &accepted);
                if (!accepted) {
                    // Never log a TURN URL: it may embed a password.
                    notifyError(QStringLiteral("GStreamer rejected a TURN server"));
                }
            }
        }

        gst_bin_add_many(GST_BIN(pipeline_), webrtc_, mixer_, volume_, valve_, input,
                         convert, resample, encoder, payloader, capsFilter,
                         playbackConvert, playback, nullptr);
        if (!gst_element_link_many(input, convert, resample, volume_, valve_,
                                   encoder, payloader, capsFilter, nullptr)
            || !gst_element_link_many(mixer_, playbackConvert, playback, nullptr)) {
            notifyError(QStringLiteral("Could not link GStreamer Opus/audio pipeline"));
            return;
        }
        GstPad* src = gst_element_get_static_pad(capsFilter, "src");
        if (!src) {
            notifyError(QStringLiteral("GStreamer RTP caps filter has no source pad"));
            return;
        }
        GstPad* sink = gst_element_request_pad_simple(webrtc_, "sink_%u");
        if (!sink) {
            gst_object_unref(src);
            notifyError(QStringLiteral("webrtcbin could not create an RTP sink pad"));
            return;
        }
        const GstPadLinkReturn linked = gst_pad_link(src, sink);
        gst_object_unref(src);
        if (linked != GST_PAD_LINK_OK) {
            gst_element_release_request_pad(webrtc_, sink);
            gst_object_unref(sink);
            notifyError(QStringLiteral("Could not link Opus RTP to webrtcbin: %1")
                            .arg(QString::fromLatin1(gst_pad_link_get_name(linked))));
            return;
        }
        gst_object_unref(sink);
        audioInput_ = input;
        audioConvert_ = convert;

        GstBus* bus = gst_element_get_bus(pipeline_);
        gst_bus_set_sync_handler(bus, onBus, this, nullptr);
        gst_object_unref(bus);
        g_signal_connect(webrtc_, "on-negotiation-needed",
                         G_CALLBACK(onNegotiationNeeded), this);
        g_signal_connect(webrtc_, "on-ice-candidate",
                         G_CALLBACK(onIceCandidate), this);
        g_signal_connect(webrtc_, "pad-added", G_CALLBACK(onRemotePad), this);
        g_signal_connect(webrtc_, "notify::connection-state",
                         G_CALLBACK(onConnectionState), this);

        // The data channel MUST be created at READY, before PLAYING.
        // This reproduces the verified one-stage Python PoC and ensures
        // the initial offer includes both audio and SCTP m-lines.
        if (gst_element_set_state(pipeline_, GST_STATE_READY)
            == GST_STATE_CHANGE_FAILURE) {
            notifyError(QStringLiteral("Could not prepare GStreamer WebRTC pipeline"));
            return;
        }
        // max-compat keeps the application m-line port nonzero for Pion v4.
        GstWebRTCDataChannel* dataChannel = nullptr;
        g_signal_emit_by_name(webrtc_, "create-data-channel", "calls-dc",
                              nullptr, &dataChannel);
        if (!dataChannel) {
            notifyError(QStringLiteral("Could not create the WebRTC DataChannel"));
            return;
        }
        g_object_unref(dataChannel);

        if (gst_element_set_state(pipeline_, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
            notifyError(QStringLiteral("Could not start GStreamer WebRTC pipeline"));
            return;
        }
        // In GStreamer 1.24 on-negotiation-needed is not reliable as the
        // sole trigger of the initial offer; explicit creation is idempotent
        // thanks to the creatingDescription_/localOfferPending_ guards.
        makeOffer();
    }

    void attachRemoteAudio(GstPad* pad)
    {
        if (!pipeline_ || !mixer_) return;
        GstCaps* caps = gst_pad_get_current_caps(pad);
        if (!caps) caps = gst_pad_query_caps(pad, nullptr);
        if (!caps || gst_caps_is_empty(caps)) {
            if (caps) gst_caps_unref(caps);
            return;
        }
        const GstStructure* structure = gst_caps_get_structure(caps, 0);
        const gchar* media = gst_structure_get_string(structure, "media");
        const gchar* encoding = gst_structure_get_string(structure, "encoding-name");
        const bool isOpus = media && encoding && g_strcmp0(media, "audio") == 0
            && g_ascii_strcasecmp(encoding, "OPUS") == 0;
        gst_caps_unref(caps);
        if (!isOpus) return;

        GstElement* queue = gst_element_factory_make("queue", nullptr);
        GstElement* depay = gst_element_factory_make("rtpopusdepay", nullptr);
        GstElement* decoder = gst_element_factory_make("opusdec", nullptr);
        GstElement* convert = gst_element_factory_make("audioconvert", nullptr);
        if (!queue || !depay || !decoder || !convert) {
            notifyError(QStringLiteral("Missing incoming Opus decoder element"));
            return;
        }
        gst_bin_add_many(GST_BIN(pipeline_), queue, depay, decoder, convert, nullptr);
        GstPad* src = gst_element_get_static_pad(convert, "src");
        GstPad* mixerSink = gst_element_request_pad_simple(mixer_, "sink_%u");
        GstPad* queueSink = gst_element_get_static_pad(queue, "sink");
        const bool linked =
            gst_element_link_many(queue, depay, decoder, convert, nullptr)
            && src && mixerSink && queueSink
            && gst_pad_link(src, mixerSink) == GST_PAD_LINK_OK
            && gst_pad_link(pad, queueSink) == GST_PAD_LINK_OK;
        if (src) gst_object_unref(src);
        if (mixerSink) gst_object_unref(mixerSink);
        if (queueSink) gst_object_unref(queueSink);
        if (!linked) {
            notifyError(QStringLiteral("Could not link incoming Opus stream"));
            return;
        }
        gst_element_sync_state_with_parent(queue);
        gst_element_sync_state_with_parent(depay);
        gst_element_sync_state_with_parent(decoder);
        gst_element_sync_state_with_parent(convert);
    }

    static void onCreatedDescription(GstPromise* promise, gpointer userData)
    {
        auto* self = static_cast<Impl*>(userData);
        const GstStructure* reply = gst_promise_get_reply(promise);
        GstWebRTCSessionDescription* description = nullptr;
        if (reply) {
            const char* name = gst_structure_has_field(reply, "offer")
                ? "offer" : "answer";
            gst_structure_get(reply, name, GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                              &description, nullptr);
        }
        gst_promise_unref(promise);
        if (!description) {
            self->notifyError(QStringLiteral("GStreamer could not create SDP"));
            return;
        }
        self->post([self, description] {
            GstPromise* pending = gst_promise_new();
            g_signal_emit_by_name(self->webrtc_, "set-local-description",
                                  description, pending);
            gst_promise_unref(pending);
            gchar* text = gst_sdp_message_as_text(description->sdp);
            self->notifyDescription(
                description->type == GST_WEBRTC_SDP_TYPE_OFFER
                    ? QStringLiteral("offer") : QStringLiteral("answer"),
                QString::fromUtf8(text ? text : ""));
            g_free(text);
            if (description->type == GST_WEBRTC_SDP_TYPE_OFFER) {
                self->localOfferPending_ = true;
            }
            self->creatingDescription_ = false;
            gst_webrtc_session_description_free(description);
        });
    }

    void makeOffer()
    {
        if (!webrtc_ || creatingDescription_ || localOfferPending_) return;
        creatingDescription_ = true;
        GstPromise* promise = gst_promise_new_with_change_func(
            onCreatedDescription, this, nullptr);
        g_signal_emit_by_name(webrtc_, "create-offer", nullptr, promise);
    }

    void makeAnswer()
    {
        if (!webrtc_ || creatingDescription_) return;
        creatingDescription_ = true;
        GstPromise* promise = gst_promise_new_with_change_func(
            onCreatedDescription, this, nullptr);
        g_signal_emit_by_name(webrtc_, "create-answer", nullptr, promise);
    }

    static void onRemoteApplied(GstPromise* promise, gpointer userData)
    {
        auto* self = static_cast<Impl*>(userData);
        const GstPromiseResult result = gst_promise_wait(promise);
        gst_promise_unref(promise);
        self->post([self, result] {
            if (result != GST_PROMISE_RESULT_REPLIED) {
                self->notifyError(QStringLiteral("Remote SDP was rejected"));
                return;
            }
            self->remoteReady_ = true;
            for (const PendingIce& ice : self->pendingIce_) {
                self->addIce(ice);
            }
            self->pendingIce_.clear();
            if (self->answerRemoteOffer_) {
                self->answerRemoteOffer_ = false;
                self->makeAnswer();
            } else {
                self->localOfferPending_ = false;
            }
        });
    }

    void receiveDescription(const QJsonObject& description)
    {
        if (!webrtc_) return;
        const QString type = description.value(QStringLiteral("type")).toString();
        QString content = description.value(QStringLiteral("sdp")).toString();
        if (type != QLatin1String("offer") && type != QLatin1String("answer")) {
            notifyError(QStringLiteral("Unknown remote SDP description type"));
            return;
        }
        // Never mutate the local GStreamer SDP: normalize received Opus casing
        // only where the server's SDP differs from GStreamer's caps.
        content.replace(QStringLiteral("opus/48000/2"),
                        QStringLiteral("OPUS/48000/2"));
        GstSDPMessage* sdp = nullptr;
        gst_sdp_message_new(&sdp);
        const QByteArray bytes = content.toUtf8();
        const GstSDPResult parsed = gst_sdp_message_parse_buffer(
            reinterpret_cast<const guint8*>(bytes.constData()),
            guint(bytes.size()), sdp);
        if (parsed != GST_SDP_OK) {
            notifyError(QStringLiteral("Invalid remote SDP"));
            gst_sdp_message_free(sdp);
            return;
        }
        const bool offer = type == QLatin1String("offer");
        GstWebRTCSessionDescription* remote = gst_webrtc_session_description_new(
            offer ? GST_WEBRTC_SDP_TYPE_OFFER : GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
        answerRemoteOffer_ = offer;
        remoteReady_ = false;
        GstPromise* promise = gst_promise_new_with_change_func(
            onRemoteApplied, this, nullptr);
        g_signal_emit_by_name(webrtc_, "set-remote-description", remote, promise);
        gst_webrtc_session_description_free(remote);
    }

    void addIce(const PendingIce& ice)
    {
        if (!webrtc_ || ice.candidate.isEmpty() || ice.mline < 0) return;
        const QByteArray utf8 = ice.candidate.toUtf8();
        g_signal_emit_by_name(webrtc_, "add-ice-candidate",
                              guint(ice.mline), utf8.constData());
    }

    void receiveCandidate(const QJsonObject& candidate)
    {
        const PendingIce ice {
            candidate.value(QStringLiteral("sdpMLineIndex")).toInt(-1),
            candidate.value(QStringLiteral("candidate")).toString()
        };
        if (remoteReady_) addIce(ice);
        else pendingIce_.push_back(ice);
    }

    // Enumerate only when requested. Device handles stay on the GLib worker
    // and no GStreamer pointers enter a Qt widget or plugin API.
    void scanAudioDevices()
    {
        for (auto it = deviceCache_.begin(); it != deviceCache_.end(); ++it) {
            gst_object_unref(it.value());
        }
        deviceCache_.clear();
        QList<CallsMediaDevice> devices;
        GstDeviceMonitor* monitor = gst_device_monitor_new();
        if (monitor) {
            gst_device_monitor_add_filter(monitor, "Audio/Source", nullptr);
            if (gst_device_monitor_start(monitor)) {
                GList* found = gst_device_monitor_get_devices(monitor);
                for (GList* cursor = found; cursor; cursor = cursor->next) {
                    auto* device = GST_DEVICE(cursor->data);
                    gchar* name = gst_device_get_display_name(device);
                    GstStructure* properties = gst_device_get_properties(device);
                    gchar* serialized = properties
                        ? gst_structure_to_string(properties) : nullptr;
                    const QString label = QString::fromUtf8(name ? name : "");
                    const QByteArray identity =
                        (label + QLatin1Char('|')
                         + QString::fromUtf8(serialized ? serialized : "")).toUtf8();
                    QString id = QStringLiteral("audio:") + QString::fromLatin1(
                        QCryptographicHash::hash(identity, QCryptographicHash::Sha256)
                            .toHex().left(20));
                    const QString base = id;
                    int collision = 1;
                    while (deviceCache_.contains(id)) {
                        id = base + QLatin1Char(':') + QString::number(collision++);
                    }
                    deviceCache_.insert(id, GST_DEVICE(gst_object_ref(device)));
                    devices.append({id,
                                    label.isEmpty() ? QStringLiteral("Microphone") : label,
                                    CallsDeviceKind::Microphone});
                    g_free(serialized);
                    if (properties) gst_structure_free(properties);
                    g_free(name);
                }
                g_list_free_full(found, reinterpret_cast<GDestroyNotify>(gst_object_unref));
                gst_device_monitor_stop(monitor);
            }
            gst_object_unref(monitor);
        }
        notifyDevices(devices);
    }

    void selectAudioInput(const QString& id)
    {
        if (!pipeline_ || !audioInput_ || !audioConvert_ || id == selectedAudioId_) {
            return;
        }
        GstElement* replacement = nullptr;
        if (id.isEmpty()) {
            const bool fake = qEnvironmentVariable("MATTERLEAST_CALLS_TEST_FAKE_AUDIO")
                == QLatin1String("1");
            replacement = gst_element_factory_make(
                fake ? "audiotestsrc" : "autoaudiosrc", nullptr);
            if (replacement && fake) {
                g_object_set(replacement, "is-live", TRUE, nullptr);
            }
        } else {
            const auto it = deviceCache_.constFind(id);
            if (it != deviceCache_.cend()) {
                replacement = gst_device_create_element(it.value(), nullptr);
            }
        }
        if (!replacement) {
            qWarning() << "MatterLeast Calls: selected microphone no longer available";
            scanAudioDevices();
            return;
        }

        // Leave the encoder, RTP transceiver, ICE and Qt participant tiles
        // intact. A source-only switch must not recreate the call or offer.
        GstElement* previous = audioInput_;
        gst_object_ref(previous);
        gst_element_set_state(previous, GST_STATE_NULL);
        gst_element_unlink(previous, audioConvert_);
        gst_bin_remove(GST_BIN(pipeline_), previous);
        const bool added = gst_bin_add(GST_BIN(pipeline_), replacement);
        const bool installed = added
            && gst_element_link(replacement, audioConvert_)
            && gst_element_sync_state_with_parent(replacement);
        if (installed) {
            audioInput_ = replacement;
            selectedAudioId_ = id;
            gst_object_unref(previous);
            scanAudioDevices();
            return;
        }

        // Restore the previous device on failure rather than leaving an
        // unlinked microphone in an otherwise connected call.
        if (added) {
            gst_element_set_state(replacement, GST_STATE_NULL);
            gst_bin_remove(GST_BIN(pipeline_), replacement);
        } else {
            gst_object_unref(replacement);
        }
        if (!gst_bin_add(GST_BIN(pipeline_), previous)
            || !gst_element_link(previous, audioConvert_)
            || !gst_element_sync_state_with_parent(previous)) {
            notifyError(QStringLiteral("Could not restore the previous microphone"));
            return;
        }
        qWarning() << "MatterLeast Calls: microphone switch failed";
        scanAudioDevices();
    }

    void setMuted(bool muted)
    {
        // Stop outgoing Opus RTP instead of transmitting encoded silence.
        // Preserve the webrtcbin sender/transceiver across mute toggles.
        if (valve_) g_object_set(valve_, "drop", gboolean(muted), nullptr);
        if (volume_) g_object_set(volume_, "mute", gboolean(muted), nullptr);
    }

    QObject* receiver_ = nullptr;
    DescriptionHandler onDescription_;
    CandidateHandler onCandidate_;
    ErrorHandler onError_;
    ConnectedHandler onConnected_;
    DevicesHandler onDevices_;
    QStringList iceServers_;
    QHash<QString, GstDevice*> deviceCache_;
    QString selectedAudioId_;
    std::atomic<bool> running_ {false};
    std::thread thread_;
    GMainContext* context_ = nullptr;
    GMainLoop* loop_ = nullptr;
    GstElement* pipeline_ = nullptr;
    GstElement* webrtc_ = nullptr;
    GstElement* mixer_ = nullptr;
    GstElement* volume_ = nullptr;
    GstElement* valve_ = nullptr;
    GstElement* audioInput_ = nullptr;
    GstElement* audioConvert_ = nullptr;
    bool creatingDescription_ = false;
    bool localOfferPending_ = false;
    bool answerRemoteOffer_ = false;
    bool remoteReady_ = false;
    std::vector<PendingIce> pendingIce_;
};

GstAudioEngine::GstAudioEngine(
    QObject* receiver, DescriptionHandler onDescription,
    CandidateHandler onCandidate, ErrorHandler onError,
    ConnectedHandler onConnected, QStringList iceServers,
    DevicesHandler onDevices)
    : d_(std::make_unique<Impl>(receiver, std::move(onDescription),
                               std::move(onCandidate), std::move(onError),
                               std::move(onConnected), std::move(iceServers),
                               std::move(onDevices)))
{
}

GstAudioEngine::~GstAudioEngine() = default;

QString GstAudioEngine::checkRuntime()
{
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        const QString reason = gstError(error);
        if (error) g_error_free(error);
        return reason;
    }
    static const char* required[] = {
        // webrtcbin can be installed without the factories it needs to
        // request an RTP sink pad or create the Calls data channel.
        "webrtcbin", "nicesrc", "nicesink", "dtlsenc", "dtlsdec", "sctpenc", "sctpdec",
        "autoaudiosrc", "autoaudiosink", "audiotestsrc", "audiomixer",
        "audioconvert", "audioresample", "volume", "valve",
        "opusenc", "rtpopuspay", "rtpopusdepay", "opusdec",
        "capsfilter", "queue"
    };
    for (const char* name : required) {
        GstElementFactory* factory = gst_element_factory_find(name);
        if (!factory) {
            return QStringLiteral("Missing GStreamer element: %1")
                .arg(QString::fromLatin1(name));
        }
        gst_object_unref(factory);
    }
    return {};
}

void GstAudioEngine::start() { d_->start(); }

void GstAudioEngine::receiveDescription(const QJsonObject& description)
{
    d_->post([self = d_.get(), description] {
        self->receiveDescription(description);
    });
}

void GstAudioEngine::receiveCandidate(const QJsonObject& candidate)
{
    d_->post([self = d_.get(), candidate] {
        self->receiveCandidate(candidate);
    });
}

void GstAudioEngine::requestDevices()
{
    d_->post([self = d_.get()] { self->scanAudioDevices(); });
}

void GstAudioEngine::setAudioInputDevice(const QString& id)
{
    d_->post([self = d_.get(), id] { self->selectAudioInput(id); });
}

void GstAudioEngine::setMicrophoneMuted(bool muted)
{
    d_->post([self = d_.get(), muted] { self->setMuted(muted); });
}

void GstAudioEngine::stop() { d_->stop(); }
