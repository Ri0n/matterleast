#include "GstAudioEngine.h"
#include "CallsMediaMap.h"

#include <atomic>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include <QCryptographicHash>
#include <QDebug>
#include <QHash>
#include <QJsonValue>
#include <QImage>
#include <QUuid>
#include <QRandomGenerator>
#include <QHash>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QVariant>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
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
         DevicesHandler onDevices,
         CameraHandler onCamera,
         VideoFrameHandler onVideoFrame,
         MediaMapHandler onMediaMap,
         VideoStreamHandler onVideoStream, bool enableVideo)
        : receiver_(receiver)
        , onDescription_(std::move(onDescription))
        , onCandidate_(std::move(onCandidate))
        , onError_(std::move(onError))
        , onConnected_(std::move(onConnected))
        , onDevices_(std::move(onDevices))
        , onCamera_(std::move(onCamera))
        , onVideoFrame_(std::move(onVideoFrame))
        , onMediaMap_(std::move(onMediaMap))
        , onVideoStream_(std::move(onVideoStream))
        , enableVideo_(enableVideo)
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
        if (enableVideo_ && type == QLatin1String("offer")) {
            // RTCD accepts an incoming camera stream only when this value
            // matches the RTP stream-id announced with video_on.
            bool inVideo = false;
            const QStringList lines = sdp.split(QLatin1Char('\n'));
            for (const QString& rawLine : lines) {
                const QString line = rawLine.trimmed();
                if (line.startsWith(QLatin1String("m="))) {
                    inVideo = line.startsWith(QLatin1String("m=video "));
                } else if (inVideo) {
                    QString msid;
                    if (line.startsWith(QLatin1String("a=msid:"))) {
                        msid = line.mid(7).section(QLatin1Char(' '), 0, 0);
                    } else if (line.startsWith(QLatin1String("a=ssrc:"))) {
                        const int marker = line.indexOf(QLatin1String(" msid:"));
                        if (marker >= 0) {
                            msid = line.mid(marker + 6).section(QLatin1Char(' '), 0, 0);
                        }
                    }
                    if (!msid.isEmpty() && msid != QLatin1String("-")) {
                        notifyVideoStreamId(msid);
                        break;
                    }
                }
            }
        }
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

    void notifyCamera(bool enabled, const QString& error = {})
    {
        if (!receiver_ || !onCamera_) return;
        QMetaObject::invokeMethod(receiver_,
            [cb = onCamera_, enabled, error] {
                if (cb) cb(enabled, error);
            }, Qt::QueuedConnection);
    }

    void notifyVideoFrame(const QString& mid, const QImage& image)
    {
        if (!receiver_ || !onVideoFrame_ || image.isNull()) return;
        QMetaObject::invokeMethod(receiver_,
            [cb = onVideoFrame_, mid, image] {
                if (cb) cb(mid, image);
            }, Qt::QueuedConnection);
    }

    void notifyVideoStreamId(const QString& id)
    {
        if (!receiver_ || !onVideoStream_) return;
        QMetaObject::invokeMethod(receiver_, [cb = onVideoStream_, id] {
            if (cb) cb(id);
        }, Qt::QueuedConnection);
    }

    void notifyMediaMap(const QHash<QString, QString>& midToSession)
    {
        if (!receiver_ || !onMediaMap_) return;
        QMetaObject::invokeMethod(receiver_,
            [cb = onMediaMap_, midToSession] {
                if (cb) cb(midToSession);
            }, Qt::QueuedConnection);
    }

    void notifyDevices(const QList<CallsMediaDevice>& devices)
    {
        if (!receiver_ || !onDevices_) return;
        QMetaObject::invokeMethod(receiver_,
            [cb = onDevices_, devices, audio = selectedAudioId_,
             video = selectedVideoId_] {
                if (cb) cb(devices, audio, video);
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
                cameraSource_ = nullptr;
                cameraConvert_ = nullptr;
                cameraValve_ = nullptr;
                videoSinkPad_ = nullptr;
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
        auto* self = static_cast<Impl*>(userData);
        self->attachRemoteAudio(pad);
        if (self->enableVideo_) self->attachRemoteVideo(pad);
    }

    static void onDataChannelMessage(GstWebRTCDataChannel*, GBytes* bytes,
                                     gpointer userData)
    {
        auto* self = static_cast<Impl*>(userData);
        if (!bytes) return;
        gsize size = 0;
        const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
        if (!data || size > 65536) return;
        const auto mapping = decodeRtcdVideoMediaMap(
            QByteArray(data, int(size)));
        if (!mapping.isEmpty()) self->notifyMediaMap(mapping);
    }

    static GstFlowReturn onVideoSample(GstAppSink* sink, gpointer userData)
    {
        auto* self = static_cast<Impl*>(userData);
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_EOS;
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstCaps* caps = gst_sample_get_caps(sample);
        GstVideoInfo info;
        if (buffer && caps && gst_video_info_from_caps(&info, caps)
            && GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_RGB) {
            GstVideoFrame frame;
            if (gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
                const auto* pixels = static_cast<const uchar*>(
                    GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
                const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
                if (pixels && stride >= int(GST_VIDEO_INFO_WIDTH(&info)) * 3) {
                    const QImage image(pixels,
                        int(GST_VIDEO_INFO_WIDTH(&info)),
                        int(GST_VIDEO_INFO_HEIGHT(&info)),
                        stride, QImage::Format_RGB888);
                    const char* id = static_cast<const char*>(
                        g_object_get_data(G_OBJECT(sink), "calls-video-mid"));
                    if (!image.isNull()
                        && (id == nullptr || g_strcmp0(id, "#local") != 0
                            || self->cameraEnabled_)) {
                        self->notifyVideoFrame(
                            id ? QString::fromUtf8(id) : QStringLiteral("#local"),
                            image.copy());
                    }
                }
                gst_video_frame_unmap(&frame);
            }
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
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

        if (enableVideo_) buildVideoPipeline();

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
        g_signal_connect(dataChannel, "on-message-data",
                         G_CALLBACK(onDataChannelMessage), this);
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

    static GstPadProbeReturn onFirstVideoRtp(GstPad*, GstPadProbeInfo* info,
                                             gpointer userData)
    {
        auto* self = static_cast<Impl*>(userData);
        if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)
            || self->videoPrimed_.exchange(true)) {
            return GST_PAD_PROBE_OK;
        }
        // The first synthetic RTP buffer is harmless before ICE/DTLS has
        // connected. It primes the RTP payloader's SSRC/caps before the
        // offer is created. Then the media gate remains closed until
        // Mattermost accepts video_on and the user opens the real camera.
        self->post([self] {
            if (self->cameraValve_ && !self->cameraEnabled_) {
                g_object_set(self->cameraValve_, "drop", TRUE, nullptr);
            }
            self->makeOffer();
        });
        return GST_PAD_PROBE_OK;
    }

    void buildVideoPipeline()
    {
        // Keep a send transceiver in the initial offer, but no camera device
        // is opened until the user enables it. Video gates before VP8 RTP
        // guarantee that an inactive camera sends no media to the SFU.
        cameraSource_ = gst_element_factory_make("videotestsrc", "camera-standby");
        cameraConvert_ = gst_element_factory_make("videoconvert", nullptr);
        GstElement* rate = gst_element_factory_make("videorate", nullptr);
        GstElement* scale = gst_element_factory_make("videoscale", nullptr);
        GstElement* rawFilter = gst_element_factory_make("capsfilter", nullptr);
        GstElement* tee = gst_element_factory_make("tee", nullptr);
        GstElement* netQueue = gst_element_factory_make("queue", nullptr);
        cameraValve_ = gst_element_factory_make("valve", "camera-rtp-gate");
        GstElement* encoder = gst_element_factory_make("vp8enc", nullptr);
        GstElement* payloader = gst_element_factory_make("rtpvp8pay", nullptr);
        GstElement* rtpFilter = gst_element_factory_make("capsfilter", nullptr);
        GstElement* previewQueue = gst_element_factory_make("queue", nullptr);
        GstElement* previewConvert = gst_element_factory_make("videoconvert", nullptr);
        GstElement* previewFilter = gst_element_factory_make("capsfilter", nullptr);
        GstElement* preview = gst_element_factory_make("appsink", "local-camera-preview");
        std::vector<GstElement*> elements = {
            cameraSource_, cameraConvert_, rate, scale, rawFilter, tee, netQueue,
            cameraValve_, encoder, payloader, rtpFilter, previewQueue,
            previewConvert, previewFilter, preview
        };
        for (auto* element : elements) {
            if (element) continue;
            for (auto* owned : elements) {
                if (owned) gst_object_unref(owned);
            }
            cameraSource_ = nullptr;
            cameraConvert_ = nullptr;
            cameraValve_ = nullptr;
            enableVideo_ = false;
            qWarning() << "MatterLeast Calls: video disabled; GStreamer element missing";
            notifyCamera(false, QStringLiteral("Missing GStreamer video element"));
            return;
        }
        g_object_set(cameraSource_, "is-live", TRUE, nullptr);
        g_object_set(cameraValve_, "drop", FALSE, nullptr);
        // Use a fixed nonzero SSRC for the lifetime of this peer connection.
        // Incoming RTCD tracks are matched to the media-stream ID in SDP.
        videoSsrc_ = QRandomGenerator::global()->generate();
        if (!videoSsrc_) videoSsrc_ = 1;
        g_object_set(payloader, "ssrc", videoSsrc_, nullptr);
        GstCaps* rawCaps = gst_caps_from_string(
            "video/x-raw,width=(int)640,height=(int)360,framerate=(fraction)15/1");
        GstCaps* rtpCaps = gst_caps_new_simple(
            "application/x-rtp",
            "media", G_TYPE_STRING, "video",
            "encoding-name", G_TYPE_STRING, "VP8",
            "clock-rate", G_TYPE_INT, 90000,
            "payload", G_TYPE_INT, 96,
            "ssrc", G_TYPE_UINT, videoSsrc_,
            nullptr);
        GstCaps* rgbCaps = gst_caps_from_string("video/x-raw,format=(string)RGB");
        g_object_set(rawFilter, "caps", rawCaps, nullptr);
        g_object_set(rtpFilter, "caps", rtpCaps, nullptr);
        g_object_set(previewFilter, "caps", rgbCaps, nullptr);
        gst_caps_unref(rawCaps);
        gst_caps_unref(rtpCaps);
        gst_caps_unref(rgbCaps);
        g_object_set(preview, "emit-signals", TRUE, "sync", FALSE,
                     "max-buffers", guint(2), "drop", TRUE, nullptr);
        g_object_set_data_full(G_OBJECT(preview), "calls-video-mid",
                               g_strdup("#local"), g_free);
        g_signal_connect(preview, "new-sample", G_CALLBACK(onVideoSample), this);
        for (auto* element : elements) gst_bin_add(GST_BIN(pipeline_), element);
        if (!gst_element_link_many(cameraSource_, cameraConvert_, rate, scale,
                                   rawFilter, tee, nullptr)
            || !gst_element_link(tee, netQueue)
            || !gst_element_link_many(netQueue, cameraValve_, encoder, payloader,
                                      rtpFilter, nullptr)
            || !gst_element_link(tee, previewQueue)
            || !gst_element_link_many(previewQueue, previewConvert,
                                      previewFilter, preview, nullptr)) {
            notifyError(QStringLiteral("Could not link GStreamer camera pipeline"));
            return;
        }
        GstPad* src = gst_element_get_static_pad(rtpFilter, "src");
        videoSinkPad_ = gst_element_request_pad_simple(webrtc_, "sink_%u");
        if (!src || !videoSinkPad_
            || gst_pad_link(src, videoSinkPad_) != GST_PAD_LINK_OK) {
            if (src) gst_object_unref(src);
            notifyError(QStringLiteral("Could not attach camera to webrtcbin"));
            return;
        }
        gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER,
                          onFirstVideoRtp, this, nullptr);
        gst_object_unref(src);
        videoStreamId_ = QStringLiteral("matterleast-video-")
            + QUuid::createUuid().toString(QUuid::WithoutBraces);
        // GStreamer >=1.22 sink-pad msid is the WebRTC MediaStream ID.
        // RTCD checks this value against video_on's videoStreamID.
        const QByteArray msid = videoStreamId_.toUtf8();
        g_object_set(videoSinkPad_, "msid", msid.constData(), nullptr);
        notifyVideoStreamId(videoStreamId_);
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

    void attachRemoteVideo(GstPad* pad)
    {
        if (!pipeline_ || !enableVideo_) return;
        GstCaps* caps = gst_pad_get_current_caps(pad);
        if (!caps) caps = gst_pad_query_caps(pad, nullptr);
        if (!caps || gst_caps_is_empty(caps)) {
            if (caps) gst_caps_unref(caps);
            return;
        }
        const GstStructure* structure = gst_caps_get_structure(caps, 0);
        const gchar* media = gst_structure_get_string(structure, "media");
        const gchar* codec = gst_structure_get_string(structure, "encoding-name");
        const bool vp8 = media && g_strcmp0(media, "video") == 0
            && codec && g_ascii_strcasecmp(codec, "VP8") == 0;
        gst_caps_unref(caps);
        if (!vp8) return;

        GstElement* queue = gst_element_factory_make("queue", nullptr);
        GstElement* depay = gst_element_factory_make("rtpvp8depay", nullptr);
        GstElement* decoder = gst_element_factory_make("vp8dec", nullptr);
        GstElement* convert = gst_element_factory_make("videoconvert", nullptr);
        GstElement* rgbFilter = gst_element_factory_make("capsfilter", nullptr);
        GstElement* sink = gst_element_factory_make("appsink", nullptr);
        std::vector<GstElement*> elements {
            queue, depay, decoder, convert, rgbFilter, sink
        };
        for (auto* element : elements) {
            if (element) continue;
            for (auto* owned : elements) {
                if (owned) gst_object_unref(owned);
            }
            notifyError(QStringLiteral("Missing incoming VP8 decoder"));
            return;
        }

        GstCaps* rgbCaps = gst_caps_from_string(
            "video/x-raw,format=(string)RGB");
        g_object_set(rgbFilter, "caps", rgbCaps, nullptr);
        gst_caps_unref(rgbCaps);
        g_object_set(sink, "emit-signals", TRUE, "sync", FALSE,
                     "max-buffers", guint(2), "drop", TRUE, nullptr);

        QString mid;
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(pad),
                                          "transceiver")) {
            GstWebRTCRTPTransceiver* transceiver = nullptr;
            g_object_get(pad, "transceiver", &transceiver, nullptr);
            if (transceiver) {
                gchar* midText = nullptr;
                g_object_get(transceiver, "mid", &midText, nullptr);
                mid = QString::fromUtf8(midText ? midText : "");
                g_free(midText);
                gst_object_unref(transceiver);
            }
        }
        if (mid.isEmpty()) {
            const QString padName = QString::fromUtf8(GST_PAD_NAME(pad));
            if (padName.startsWith(QLatin1String("src_"))) {
                mid = padName.mid(4);
            }
        }
        g_object_set_data_full(G_OBJECT(sink), "calls-video-mid",
                               g_strdup(mid.toUtf8().constData()), g_free);
        g_signal_connect(sink, "new-sample", G_CALLBACK(onVideoSample), this);

        for (auto* element : elements) gst_bin_add(GST_BIN(pipeline_), element);
        GstPad* queueSink = gst_element_get_static_pad(queue, "sink");
        const bool linked = queueSink
            && gst_element_link_many(queue, depay, decoder, convert,
                                     rgbFilter, sink, nullptr)
            && gst_pad_link(pad, queueSink) == GST_PAD_LINK_OK;
        if (queueSink) gst_object_unref(queueSink);
        if (!linked) {
            notifyError(QStringLiteral("Could not link incoming VP8 video"));
            return;
        }
        for (auto* element : elements) gst_element_sync_state_with_parent(element);
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
        if (!webrtc_ || creatingDescription_ || localOfferPending_
            || (enableVideo_ && !videoPrimed_)) return;
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
    void scanMediaDevices()
    {
        for (auto it = deviceCache_.begin(); it != deviceCache_.end(); ++it) {
            gst_object_unref(it.value());
        }
        deviceCache_.clear();
        QList<CallsMediaDevice> devices;
        // More than one GstDeviceProvider may enumerate the same physical
        // device, with different properties and IDs. Group those aliases by
        // device kind and normalized user-visible name. Pick a stable source
        // with a platform media provider preferred over raw ALSA.
        QHash<QString, int> logicalDeviceIndex;
        QHash<QString, int> logicalDevicePriority;
        GstDeviceMonitor* monitor = gst_device_monitor_new();
        if (monitor) {
            gst_device_monitor_add_filter(monitor, "Audio/Source", nullptr);
            gst_device_monitor_add_filter(monitor, "Video/Source", nullptr);
            if (gst_device_monitor_start(monitor)) {
                GList* found = gst_device_monitor_get_devices(monitor);
                for (GList* cursor = found; cursor; cursor = cursor->next) {
                    auto* device = GST_DEVICE(cursor->data);
                    gchar* name = gst_device_get_display_name(device);
                    GstStructure* properties = gst_device_get_properties(device);
                    gchar* serialized = properties
                        ? gst_structure_to_string(properties) : nullptr;
                    const QString label = QString::fromUtf8(name ? name : "");
                    gchar* rawClass = gst_device_get_device_class(device);
                    const QString deviceClass = QString::fromUtf8(rawClass ? rawClass : "");
                    const bool isCamera = deviceClass.contains(QStringLiteral("Video/Source"));
                    const CallsDeviceKind kind = isCamera ? CallsDeviceKind::Camera
                                                          : CallsDeviceKind::Microphone;
                    const QByteArray identity =
                        (deviceClass + QLatin1Char('|') + label + QLatin1Char('|')
                         + QString::fromUtf8(serialized ? serialized : "")).toUtf8();
                    QString id = (isCamera ? QStringLiteral("video:")
                                            : QStringLiteral("audio:"))
                        + QString::fromLatin1(
                        QCryptographicHash::hash(identity, QCryptographicHash::Sha256)
                            .toHex().left(20));
                    const QString displayName = label.isEmpty()
                        ? (isCamera ? QStringLiteral("Camera")
                                    : QStringLiteral("Microphone"))
                        : label;
                    const QString logicalKey =
                        QString::number(int(kind)) + QLatin1Char('|')
                        + displayName.simplified().toCaseFolded();
                    const QString provider =
                        (QString::fromLatin1(G_OBJECT_TYPE_NAME(device))
                         + QLatin1Char(' ')
                         + QString::fromUtf8(serialized ? serialized : ""))
                            .toCaseFolded();
                    // An existing user choice wins over backend preference
                    // when the same logical device is enumerated twice.
                    const int score =
                        (id == selectedAudioId_ || id == selectedVideoId_ ? 1000 : 0)
                        + (provider.contains(QLatin1String("pipewire")) ? 70
                           : provider.contains(QLatin1String("pulse")) ? 60
                           : provider.contains(QLatin1String("wasapi")) ? 60
                           : provider.contains(QLatin1String("alsa")) ? 20 : 0);
                    const auto existing = logicalDeviceIndex.constFind(logicalKey);
                    if (existing != logicalDeviceIndex.cend()) {
                        const int index = existing.value();
                        const int oldScore = logicalDevicePriority.value(logicalKey);
                        const QString previousId = devices.at(index).id;
                        if (score < oldScore
                            || (score == oldScore && id >= previousId)) {
                            g_free(rawClass);
                            g_free(serialized);
                            if (properties) gst_structure_free(properties);
                            g_free(name);
                            continue;
                        }
                        gst_object_unref(deviceCache_.take(previousId));
                        devices[index] = {id, displayName, kind};
                        logicalDevicePriority[logicalKey] = score;
                    } else {
                        logicalDeviceIndex.insert(logicalKey, devices.size());
                        logicalDevicePriority.insert(logicalKey, score);
                        devices.append({id, displayName, kind});
                    }
                    deviceCache_.insert(id, GST_DEVICE(gst_object_ref(device)));
                    g_free(rawClass);
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
            scanMediaDevices();
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
            scanMediaDevices();
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
        scanMediaDevices();
    }

    GstElement* createCameraSource(const QString& id, bool live)
    {
        if (!live) {
            GstElement* standby = gst_element_factory_make("videotestsrc", nullptr);
            if (standby) {
                g_object_set(standby, "is-live", TRUE, nullptr);
            }
            return standby;
        }
        if (!id.isEmpty()) {
            const auto device = deviceCache_.constFind(id);
            return device == deviceCache_.cend() ? nullptr
                : gst_device_create_element(device.value(), nullptr);
        }
        const bool fake = qEnvironmentVariable("MATTERLEAST_CALLS_TEST_FAKE_VIDEO")
            == QLatin1String("1");
        GstElement* source = gst_element_factory_make(
            fake ? "videotestsrc" : "autovideosrc", nullptr);
        if (source && fake) g_object_set(source, "is-live", TRUE, nullptr);
        return source;
    }

    bool replaceCameraSource(const QString& id, bool live)
    {
        if (!pipeline_ || !cameraSource_ || !cameraConvert_) return false;
        GstElement* replacement = createCameraSource(id, live);
        if (!replacement) return false;

        GstElement* previous = cameraSource_;
        gst_object_ref(previous);
        gst_element_set_state(previous, GST_STATE_NULL);
        gst_element_unlink(previous, cameraConvert_);
        gst_bin_remove(GST_BIN(pipeline_), previous);
        const bool added = gst_bin_add(GST_BIN(pipeline_), replacement);
        const bool installed = added
            && gst_element_link(replacement, cameraConvert_)
            && gst_element_sync_state_with_parent(replacement);
        if (installed) {
            cameraSource_ = replacement;
            gst_object_unref(previous);
            return true;
        }

        if (added) {
            gst_element_set_state(replacement, GST_STATE_NULL);
            gst_bin_remove(GST_BIN(pipeline_), replacement);
        } else {
            gst_object_unref(replacement);
        }
        if (!gst_bin_add(GST_BIN(pipeline_), previous)
            || !gst_element_link(previous, cameraConvert_)
            || !gst_element_sync_state_with_parent(previous)) {
            notifyError(QStringLiteral("Could not restore the previous camera"));
        }
        return false;
    }

    void selectVideoInput(const QString& id)
    {
        if (id == selectedVideoId_) return;
        if (!id.isEmpty() && !deviceCache_.contains(id)) {
            scanMediaDevices();
            return;
        }
        if (cameraEnabled_ && !replaceCameraSource(id, true)) {
            notifyCamera(true, QStringLiteral("Could not switch camera"));
            scanMediaDevices();
            return;
        }
        selectedVideoId_ = id;
        scanMediaDevices();
    }

    void toggleCamera(bool enabled)
    {
        if (!enableVideo_ || !cameraValve_) {
            notifyCamera(false, QStringLiteral("Camera is unavailable"));
            return;
        }
        if (enabled == cameraEnabled_) return;
        if (enabled) {
            // Keep the RTP gate closed until Mattermost accepts the
            // video_on signal with the exact WebRTC msid.
            if (!replaceCameraSource(selectedVideoId_, true)) {
                notifyCamera(false, QStringLiteral("Could not start the camera"));
                return;
            }
            cameraEnabled_ = true;
            notifyCamera(true);
        } else {
            g_object_set(cameraValve_, "drop", TRUE, nullptr);
            cameraEnabled_ = false;
            if (!replaceCameraSource({}, false)) {
                notifyCamera(false, QStringLiteral("Could not release the camera"));
                return;
            }
            notifyCamera(false);
        }
    }

    void setCameraPublishing(bool enabled)
    {
        if (cameraValve_) {
            g_object_set(cameraValve_, "drop",
                         gboolean(!enabled || !cameraEnabled_), nullptr);
        }
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
    CameraHandler onCamera_;
    VideoFrameHandler onVideoFrame_;
    MediaMapHandler onMediaMap_;
    VideoStreamHandler onVideoStream_;
    bool enableVideo_ = false;
    QStringList iceServers_;
    QHash<QString, GstDevice*> deviceCache_;
    QString selectedAudioId_;
    QString selectedVideoId_;
    std::atomic<bool> cameraEnabled_ {false};
    QString videoStreamId_;
    guint videoSsrc_ = 0;
    std::atomic<bool> videoPrimed_ {false};
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
    GstElement* cameraSource_ = nullptr;
    GstElement* cameraConvert_ = nullptr;
    GstElement* cameraValve_ = nullptr;
    GstPad* videoSinkPad_ = nullptr;
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
    DevicesHandler onDevices, CameraHandler onCamera,
    VideoFrameHandler onVideoFrame, MediaMapHandler onMediaMap,
    VideoStreamHandler onVideoStream, bool enableVideo)
    : d_(std::make_unique<Impl>(receiver, std::move(onDescription),
                               std::move(onCandidate), std::move(onError),
                               std::move(onConnected), std::move(iceServers),
                               std::move(onDevices), std::move(onCamera),
                               std::move(onVideoFrame), std::move(onMediaMap),
                               std::move(onVideoStream), enableVideo))
{
}

GstAudioEngine::~GstAudioEngine() = default;

QString GstAudioEngine::checkVideoRuntime()
{
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
        const QString reason = gstError(error);
        if (error) g_error_free(error);
        return reason;
    }
    static const char* required[] = {
        "videotestsrc", "autovideosrc", "videoconvert", "videorate",
        "videoscale", "tee", "vp8enc", "vp8dec", "rtpvp8pay",
        "rtpvp8depay", "appsink", "capsfilter", "queue", "valve"
    };
    for (const char* name : required) {
        GstElementFactory* factory = gst_element_factory_find(name);
        if (!factory) {
            return QStringLiteral("Missing GStreamer video element: %1")
                .arg(QString::fromLatin1(name));
        }
        gst_object_unref(factory);
    }
    return {};
}

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
    d_->post([self = d_.get()] { self->scanMediaDevices(); });
}

void GstAudioEngine::setAudioInputDevice(const QString& id)
{
    d_->post([self = d_.get(), id] { self->selectAudioInput(id); });
}

void GstAudioEngine::setVideoInputDevice(const QString& id)
{
    d_->post([self = d_.get(), id] { self->selectVideoInput(id); });
}

void GstAudioEngine::setCameraEnabled(bool enabled)
{
    d_->post([self = d_.get(), enabled] { self->toggleCamera(enabled); });
}

void GstAudioEngine::setCameraPublishing(bool enabled)
{
    d_->post([self = d_.get(), enabled] { self->setCameraPublishing(enabled); });
}

void GstAudioEngine::setMicrophoneMuted(bool muted)
{
    d_->post([self = d_.get(), muted] { self->setMuted(muted); });
}

void GstAudioEngine::stop() { d_->stop(); }
