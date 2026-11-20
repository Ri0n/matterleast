#!/usr/bin/python3
"""Real RTCD ↔ GStreamer webrtcbin interop test (no Mattermost server needed).

The RTCD WebSocket protocol here is the *internal* Calls-to-RTCD protocol:
MessagePack-encoded consecutive message type + payload values.
It is NOT the Mattermost client-side Calls signaling API.

Checks: RTCD authentication, session creation, SDP audio/Opus and SCTP m-lines,
trickle ICE, DTLS/SCTP DataChannel open, and continuing synthetic audio RTP.
"""
import base64
import faulthandler
import json
import os
import secrets
import sys
import threading
import time
import urllib.request
from pathlib import Path

import msgpack
import websocket

import gi

gi.require_version("Gst", "1.0")
gi.require_version("GstSdp", "1.0")
gi.require_version("GstWebRTC", "1.0")
from gi.repository import GLib, Gst, GstSdp, GstWebRTC  # noqa: E402


RTCD_URL = os.environ.get("RTCD_URL", "http://127.0.0.1:8045")
CLIENT_ID = os.environ.get("RTCD_CLIENT_ID", "matterleast-ci-poc")
CALL_ID = "matterleast-poc-call"
PARTICIPANT = os.environ.get("RTCD_PARTICIPANT", "")
CONFERENCE = bool(PARTICIPANT)
# Default: offer Opus and SCTP together using max-compat BUNDLE, with both
# m-lines having nonzero ports. Keep the two-stage negotiation for A/B tests.
SINGLE_STAGE = os.environ.get("RTCD_SINGLE_STAGE", "1") == "1"
STATE_DIR = Path(os.environ["RTCD_STATE_DIR"]) if CONFERENCE else None
SESSION_ID = "matterleast-poc-session" + ("-" + PARTICIPANT if CONFERENCE else "")
USER_ID = "matterleast-poc-user" + ("-" + PARTICIPANT if CONFERENCE else "")
TIMEOUT_SECONDS = 90 if CONFERENCE else 45


def encode_message(kind, payload):
    # RTCD's Go ClientMessage.EncodeMsgpack uses EncodeMulti, not an array.
    return msgpack.packb(kind, use_bin_type=True) + msgpack.packb(
        payload, use_bin_type=True
    )


def decode_message(raw):
    unpacker = msgpack.Unpacker(raw=False)
    unpacker.feed(raw)
    items = list(unpacker)
    if len(items) != 2 or not isinstance(items[0], str):
        raise ValueError("invalid RTCD WebSocket frame")
    return items[0], items[1]


class RTCDWebRTCPoC:
    def __init__(self):
        self.loop = GLib.MainLoop()
        self.ws = None
        self.ws_lock = threading.Lock()
        self.stop = threading.Event()
        self.pipeline = None
        self.pc = None
        self.dc = None
        self.offer_sent = False
        self.offer_count = 0
        self.answer_count = 0
        self.offer_pending = False
        self.pending_offer_sdp = None
        self.answer_received = False
        self.audio_added = False
        self.audio_negotiated = False
        self.audio_bin = None
        self.dc_open = False
        self.remote_audio_buffers = 0
        # Count successfully decoded PCM *per independent incoming RTP pad*.
        # An aggregate counter could pass a 3-person test even if we receive
        # two copies of one peer and nothing from the other.
        self.remote_audio_tracks = {}
        self.remote_audio_complete = set()
        self.next_remote_track_id = 0
        self.continuity_baseline = None
        self.incoming_audio_bins = []
        self.pending_server_offers = []
        self.responding_to_offer = False
        self.server_answers_sent = 0
        self.rtp_packets = 0
        self.rtp_at_open = 0
        self.error = None
        self.started = time.monotonic()

    def log(self, message):
        print("[rtcd-webrtcbin] " + message, flush=True)

    def mark(self, state):
        if CONFERENCE:
            (STATE_DIR / (PARTICIPANT + "-" + state)).write_text(
                str(time.monotonic()), encoding="utf-8"
            )

    def fail(self, message):
        if self.error is None:
            self.error = str(message)
            self.log("FAIL: " + self.error)
        self.loop.quit()
        return GLib.SOURCE_REMOVE

    def send(self, kind, payload):
        with self.ws_lock:
            self.ws.send(encode_message(kind, payload), opcode=websocket.ABNF.OPCODE_BINARY)

    def send_rtc(self, rtc_type, data):
        self.send("rtc", {
            "group_id": CLIENT_ID,
            "call_id": CALL_ID,
            "session_id": SESSION_ID,
            "user_id": USER_ID,
            "type": rtc_type,
            "data": json.dumps(data, separators=(",", ":")).encode("utf-8"),
        })

    def connect_rtcd(self):
        auth_key = os.environ.get("RTCD_AUTH_KEY")
        if not auth_key:
            auth_key = secrets.token_urlsafe(36)
            request = urllib.request.Request(
                RTCD_URL + "/register",
                json.dumps({"clientID": CLIENT_ID, "authKey": auth_key}).encode(),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            with urllib.request.urlopen(request, timeout=10) as resp:
                if resp.status != 201:
                    raise RuntimeError("RTCD registration status: " + str(resp.status))
            self.log("registered test client")
        else:
            self.log("using shared conference client credentials")

        auth = base64.b64encode((CLIENT_ID + ":" + auth_key).encode()).decode()
        self.ws = websocket.create_connection(
            RTCD_URL.replace("http://", "ws://", 1).replace("https://", "wss://", 1)
            + "/ws",
            timeout=10, header=["Authorization: Basic " + auth],
            enable_multithread=True,
        )
        kind, payload = decode_message(self.ws.recv())
        if kind != "hello" or payload.get("clientID") != CLIENT_ID:
            raise RuntimeError("RTCD hello mismatch: " + repr((kind, payload)))
        self.log("WebSocket authenticated, connID=" + payload.get("connID", "?"))
        self.send("join", {
            "callID": CALL_ID,
            "userID": USER_ID,
            "sessionID": SESSION_ID,
            "channelID": "matterleast-ci",
            "dcSignaling": False,
            "av1Support": False,
        })
        self.log("created RTCD RTC session")

    def receive_ws(self):
        while not self.stop.is_set():
            try:
                frame = self.ws.recv()
                if not frame:
                    break
                kind, payload = decode_message(frame)
                GLib.idle_add(self.on_ws_message, kind, payload)
            except websocket.WebSocketTimeoutException:
                continue
            except Exception as exc:
                if not self.stop.is_set():
                    GLib.idle_add(self.fail, "WebSocket receive: " + repr(exc))
                return
        if not self.stop.is_set():
            GLib.idle_add(self.fail, "RTCD closed WebSocket before completion")

    def on_ws_message(self, kind, payload):
        try:
            if kind == "close":
                return self.fail("RTCD closed session: " + repr(payload))
            if kind != "rtc":
                self.log("RTCD event " + kind)
                return GLib.SOURCE_REMOVE

            msg_type = payload["type"]
            data = json.loads(payload["data"])
            if msg_type == 1:
                candidate = data.get("candidate", data)
                if isinstance(candidate, dict):
                    line = candidate.get("candidate", "")
                    index = candidate.get("sdpMLineIndex", 0) or 0
                    if line:
                        self.pc.emit("add-ice-candidate", int(index), line)
                return GLib.SOURCE_REMOVE
            if msg_type != 2:
                return GLib.SOURCE_REMOVE

            sdp = data["sdp"]
            self.log("answer media: " + " | ".join(line for line in sdp.splitlines() if line.startswith(("m=", "a=sctp-port:", "a=setup:", "a=group:BUNDLE"))))
            sdp_type = data["type"]
            if sdp_type == "offer":
                self.pending_server_offers.append(sdp)
                self.log("received RTCD server offer; queue=%d" %
                         len(self.pending_server_offers))
                self.process_server_offers()
                return GLib.SOURCE_REMOVE
            if sdp_type != "answer":
                return self.fail("unexpected remote SDP: " + sdp_type)
            required_media = ["m=application ", "a=fingerprint:", "a=sctp-port:"]
            if self.audio_added:
                required_media.extend(("m=audio ", "opus/48000/2"))
            for required in required_media:
                if required not in sdp.lower():
                    return self.fail("answer missing " + required)
            if self.offer_count == 1:
                app_line = next((line for line in sdp.splitlines()
                                 if line.startswith("m=application ")), "")
                if app_line.split()[1] == "0":
                    return self.fail("RTCD rejected initial DataChannel")
            # Pion may use the lowercase MIME name from our wire offer
            # in its answer; GStreamer 1.24 requires canonical uppercase
            # OPUS caps when applying its remote answer.
            local_sdp = sdp.replace(
                "a=rtpmap:111 opus/48000/2",
                "a=rtpmap:111 OPUS/48000/2",
            )
            result, sdp_msg = GstSdp.SDPMessage.new_from_text(local_sdp)
            if result != GstSdp.SDPResult.OK:
                return self.fail("could not parse RTCD SDP")
            answer = GstWebRTC.WebRTCSessionDescription.new(
                GstWebRTC.WebRTCSDPType.ANSWER, sdp_msg
            )
            promise = Gst.Promise.new_with_change_func(
                self.on_remote_description_set, None, None
            )
            self.pc.emit("set-remote-description", answer, promise)
            self.log("received RTCD SDP answer; awaiting GStreamer validation")
        except Exception as exc:
            return self.fail("handling RTCD signaling: " + repr(exc))
        return GLib.SOURCE_REMOVE

    def check_description_promise(self, promise, action):
        if promise.wait() != Gst.PromiseResult.REPLIED:
            raise RuntimeError(action + " did not reply")
        reply = promise.get_reply()
        if reply is not None and reply.has_field("error"):
            raise RuntimeError(action + ": " + str(reply.get_value("error")))

    def on_remote_description_set(self, promise, _user_data, _unused):
        try:
            self.check_description_promise(promise, "set-remote-description")
            GLib.idle_add(self.mark_answer_received)
        except Exception as exc:
            GLib.idle_add(self.fail, repr(exc))

    def mark_answer_received(self):
        self.answer_received = True
        self.answer_count += 1
        self.offer_pending = False
        if self.audio_added:
            # The payloader's probe permits RTP only after the SFU has
            # accepted the audio SDP. Sending before SetRemoteDescription
            # can create an undeclared SSRC and a prematurely closed
            # Pion RTPReceiver during renegotiation.
            self.audio_negotiated = True
            self.log("audio SDP applied; allowing Opus RTP into webrtcbin")
            self.mark("publishing")
        self.log("GStreamer accepted RTCD SDP answer #%d" % self.answer_count)
        self.process_server_offers()
        if self.dc_open and not self.audio_added and not CONFERENCE:
            return self.start_audio()
        return GLib.SOURCE_REMOVE

    def on_local_description_set(self, promise, _user_data, _unused):
        try:
            self.check_description_promise(promise, "set-local-description")
            GLib.idle_add(self.send_pending_offer)
        except Exception as exc:
            GLib.idle_add(self.fail, repr(exc))

    def send_pending_offer(self):
        try:
            # RTCD v1.2.6 checks remoteTrack.Codec().MimeType against
            # "audio/opus" with Go's case-sensitive ==. GStreamer correctly
            # uses "OPUS" (RFC 7587); the Pion SDP parser preserves the case.
            # Normalize only the outgoing signaling copy, leaving the local
            # webrtcbin description and RTP caps canonical. SDP codec names
            # are case-insensitive; this does not alter transport semantics.
            wire_sdp = self.pending_offer_sdp.replace(
                "a=rtpmap:111 OPUS/48000/2",
                "a=rtpmap:111 opus/48000/2",
            )
            if self.audio_added and wire_sdp == self.pending_offer_sdp:
                raise RuntimeError("expected an OPUS RTP map to normalize")
            self.send_rtc(2, {"type": "offer", "sdp": wire_sdp})
            self.offer_sent = True
            self.offer_count += 1
            self.log("sent SDP offer #%d (%s)" %
                     (self.offer_count, "audio + SCTP" if self.audio_added
                      else "SCTP only"))
        except Exception as exc:
            return self.fail("sending offer: " + repr(exc))
        return GLib.SOURCE_REMOVE

    def process_server_offers(self):
        if not self.pending_server_offers or self.offer_pending or self.responding_to_offer:
            return
        if self.pc.get_property("signaling-state") != GstWebRTC.WebRTCSignalingState.STABLE:
            return
        sdp = self.pending_server_offers.pop(0)
        self.responding_to_offer = True
        result, message = GstSdp.SDPMessage.new_from_text(sdp)
        if result != GstSdp.SDPResult.OK:
            return self.fail("cannot parse server offer")
        remote = GstWebRTC.WebRTCSessionDescription.new(
            GstWebRTC.WebRTCSDPType.OFFER, message
        )
        promise = Gst.Promise.new_with_change_func(
            self.on_server_offer_set, None, None
        )
        self.pc.emit("set-remote-description", remote, promise)
        self.log("applying RTCD-initiated offer for remote conference track")
        return GLib.SOURCE_REMOVE

    def on_server_offer_set(self, promise, _user_data, _unused):
        try:
            self.check_description_promise(promise, "server set-remote-offer")
            GLib.idle_add(self.create_server_answer)
        except Exception as exc:
            GLib.idle_add(self.fail, repr(exc))

    def create_server_answer(self):
        answer_promise = Gst.Promise.new_with_change_func(
            self.on_server_answer_created, None, None
        )
        self.pc.emit("create-answer", None, answer_promise)
        return GLib.SOURCE_REMOVE

    def on_server_answer_created(self, promise, _user_data, _unused):
        try:
            if promise.wait() != Gst.PromiseResult.REPLIED:
                raise RuntimeError("create-answer did not reply")
            reply = promise.get_reply()
            answer = reply.get_value("answer")
            if answer is None or answer.sdp is None:
                raise RuntimeError("RTCD offer generated no answer")
            # Read boxed SDP while the GstPromise reply still owns the object.
            self.pending_server_answer_sdp = answer.sdp.as_text()
            local = Gst.Promise.new_with_change_func(
                self.on_server_answer_set, None, None
            )
            self.pc.emit("set-local-description", answer, local)
        except Exception as exc:
            GLib.idle_add(self.fail, "creating RTCD answer: " + repr(exc))

    def on_server_answer_set(self, promise, _user_data, _unused):
        try:
            self.check_description_promise(promise, "server set-local-answer")
            GLib.idle_add(self.send_server_answer)
        except Exception as exc:
            GLib.idle_add(self.fail, repr(exc))

    def send_server_answer(self):
        try:
            self.send_rtc(2, {
                "type": "answer", "sdp": self.pending_server_answer_sdp
            })
            self.server_answers_sent += 1
            self.log("answered RTCD-initiated offer #%d" %
                     self.server_answers_sent)
            self.responding_to_offer = False
            self.process_server_offers()
        except Exception as exc:
            return self.fail("sending server-offer answer: " + repr(exc))
        return GLib.SOURCE_REMOVE

    def on_offer(self, promise, _user_data, _unused):
        try:
            self.log("create-offer callback entered")
            if promise.wait() != Gst.PromiseResult.REPLIED:
                raise RuntimeError("create-offer did not reply")
            reply = promise.get_reply()  # Keep the GstStructure alive.
            offer = reply.get_value("offer")
            self.log("create-offer returned offer=%r" % (offer,))
            if offer is None:
                raise RuntimeError("create-offer returned no SDP")
            # A GstWebRTCSessionDescription is boxed: PyGObject may consume
            # its borrowed SDP when passed to set-local-description. Read the
            # offer before emitting that action; never keep a borrowed SDP.
            desc_sdp = offer.sdp
            self.log("offer SDP object=%r" % (desc_sdp,))
            if desc_sdp is None:
                raise RuntimeError("create-offer returned a null SDP")
            sdp = desc_sdp.as_text()
            self.log("offer SDP length=%d" % len(sdp))
            self.log("offer media: " + " | ".join(line for line in sdp.splitlines() if line.startswith(("m=", "a=sctp-port:", "a=setup:", "a=group:BUNDLE", "a=ssrc:", "a=rtpmap:", "a=send", "a=recv"))))
            required_media = ["m=application ", "a=sctp-port:"]
            if self.audio_added:
                required_media.extend(("m=audio ", "opus/48000/2"))
            for required in required_media:
                if required not in sdp.lower():
                    raise RuntimeError("offer missing " + required)
            if self.audio_added and not any(
                    line.startswith("a=ssrc:") for line in sdp.splitlines()):
                raise RuntimeError("audio SDP missing a=ssrc; RTCD cannot map track")
            if self.audio_added and "a=rtpmap:111 OPUS/48000/2" not in sdp:
                raise RuntimeError("GStreamer local Opus SDP lost canonical OPUS naming")
            if self.offer_count == 0:
                app_line = next(line for line in sdp.splitlines()
                                if line.startswith("m=application "))
                # Pion v4.2.6 checks the application m-line port before
                # starting SCTP, without accounting for bundle-only.
                if app_line.split()[1] == "0":
                    raise RuntimeError("initial SCTP m-line has zero port")
                if SINGLE_STAGE:
                    audio_line = next(line for line in sdp.splitlines()
                                      if line.startswith("m=audio "))
                    if audio_line.split()[1] == "0":
                        raise RuntimeError("max-compat initial Opus m-line has zero port")
            self.log("setting local description")
            self.pending_offer_sdp = sdp
            local = Gst.Promise.new_with_change_func(
                self.on_local_description_set, None, None
            )
            self.pc.emit("set-local-description", offer, local)
        except Exception as exc:
            GLib.idle_add(self.fail, "creating offer: " + repr(exc))

    def on_negotiation_needed(self, _pc):
        # Pion v4.2.6 does not start SCTP for an application m-line with
        # port zero, even with bundle-only. The default max-compat policy
        # avoids that; the optional two-stage mode puts SCTP first.
        if (self.dc is None or self.offer_pending or self.responding_to_offer
                or self.pending_server_offers):
            return
        if self.pc.get_property("signaling-state") != GstWebRTC.WebRTCSignalingState.STABLE:
            return
        self.offer_pending = True
        promise = Gst.Promise.new_with_change_func(self.on_offer, None, None)
        self.pc.emit("create-offer", None, promise)

    def on_local_ice(self, _pc, index, candidate):
        if not candidate:
            return
        try:
            self.send_rtc(1, {
                "candidate": candidate,
                "sdpMLineIndex": index,
                "sdpMid": str(index),
            })
        except Exception as exc:
            GLib.idle_add(self.fail, "sending ICE: " + repr(exc))

    def on_pc_property(self, obj, pspec):
        self.log("webrtcbin %s=%s" % (pspec.name, obj.get_property(pspec.name)))

    def on_data_channel_open(self, _channel):
        GLib.idle_add(self.mark_dc_open)

    def mark_dc_open(self):
        self.dc_open = True
        self.rtp_at_open = self.rtp_packets
        self.log("calls-dc OPEN: ICE + DTLS + SCTP established")
        self.mark("dc-ready")
        if self.answer_count > 0 and not self.audio_added and not CONFERENCE:
            return self.start_audio()
        return GLib.SOURCE_REMOVE

    def start_audio(self):
        if self.audio_added:
            return GLib.SOURCE_REMOVE
        try:
            # Stable sender SSRC is required for a valid track identity in
            # dynamically generated SDP. Without it GStreamer warns 'missing
            # ssrc', and RTCD repeatedly constructs short-lived OnTrack items.
            ssrc = 0x49100000 + (1 if PARTICIPANT == "alice" else
                                  2 if PARTICIPANT == "bob" else 3)
            description = (
                "audiotestsrc is-live=true wave=sine volume=0.03 ! "
                "audioconvert ! audioresample ! opusenc ! "
                "rtpopuspay name=pay pt=111 ssrc=%d ! " % ssrc +
                # Keep the canonical GStreamer caps OPUS, because GStreamer
                # 1.24 case-sensitively compares its own codec preferences
                # with server-initiated offers containing OPUS.
                "capsfilter caps=\"application/x-rtp,media=(string)audio,"
                "encoding-name=(string)OPUS,payload=(int)111,"
                "clock-rate=(int)48000,encoding-params=(string)2,"
                "sprop-stereo=(string)0,sprop-maxcapturerate=(string)48000,"
                "ssrc=(uint)%d\"" % ssrc
            )
            self.audio_bin = Gst.parse_bin_from_description(description, True)
            self.pipeline.add(self.audio_bin)
            pay = self.audio_bin.get_by_name("pay")
            pay.get_static_pad("src").add_probe(
                Gst.PadProbeType.BUFFER, self.on_rtp
            )
            sink = self.pc.request_pad_simple("sink_%u")
            src = self.audio_bin.get_static_pad("src")
            if sink is None or src is None or src.link(sink) != Gst.PadLinkReturn.OK:
                raise RuntimeError("could not link Opus sender to webrtcbin")
            self.audio_added = True
            if not self.audio_bin.sync_state_with_parent():
                raise RuntimeError("could not start added Opus pipeline")
            self.log("added Opus track (%s)" %
                     ("initial offer" if SINGLE_STAGE else "pending renegotiation"))
        except Exception as exc:
            return self.fail("starting Opus after DataChannel: " + repr(exc))
        return GLib.SOURCE_REMOVE

    def on_remote_pad(self, _pc, pad):
        # GstBin emits pad-added for requested sender sink pads too.
        # Only a new SRC pad can carry an incoming SFU stream.
        if pad.get_direction() != Gst.PadDirection.SRC:
            return
        # Link synchronously before returning from pad-added. Deferring to a
        # GLib idle callback lets the incoming RTP buffer reach an unlinked
        # pad, which propagates GST_FLOW_NOT_LINKED back to nicesrc and
        # permanently kills the ICE receive stream.
        self.attach_remote_audio(pad)

    def attach_remote_audio(self, pad):
        caps = pad.get_current_caps() or pad.query_caps(None)
        if not caps or caps.get_size() == 0:
            return self.fail("remote RTP pad has no caps")
        structure = caps.get_structure(0)
        # On GStreamer 1.24 the newly exposed receiving pad may initially
        # advertise generic application/x-rtp. Its final audio caps arrive
        # only after SDP/SSRC mapping. This conference PoC is audio-only;
        # connect the Opus decoder now instead of discarding the pad forever.
        if (structure.get_name() != "application/x-rtp"
                or structure.get_string("media") not in (None, "audio")):
            self.log("ignoring non-audio incoming track: %s" % caps.to_string())
            return GLib.SOURCE_REMOVE
        try:
            self.next_remote_track_id += 1
            track_id = self.next_remote_track_id
            receiver = Gst.parse_bin_from_description(
                "queue ! rtpopusdepay ! opusdec ! "
                "audioconvert ! fakesink name=decoded sync=false",
                True
            )
            sink = receiver.get_static_pad("sink")
            decoded = receiver.get_by_name("decoded")
            # Each receiver must independently decode >=10 buffers. Keep
            # the pad linked synchronously before the first RTP packet.
            def on_decoded(_pad, _info):
                return self.on_decoded_audio(track_id)
            decoded.get_static_pad("sink").add_probe(
                Gst.PadProbeType.BUFFER, on_decoded
            )
            self.remote_audio_tracks[track_id] = 0
            self.pipeline.add(receiver)
            self.incoming_audio_bins.append(receiver)
            if pad.link(sink) != Gst.PadLinkReturn.OK:
                raise RuntimeError("failed to link remote Opus track")
            if not receiver.sync_state_with_parent():
                raise RuntimeError("failed to start Opus decoder")
            self.log("attached remote Opus decoder #%d on %s: %s" %
                     (track_id, pad.get_name(), caps.to_string()))
        except Exception as exc:
            return self.fail("attaching incoming audio: " + repr(exc))
        return GLib.SOURCE_REMOVE

    def on_decoded_audio(self, track_id):
        self.remote_audio_tracks[track_id] += 1
        self.remote_audio_buffers += 1
        if self.remote_audio_tracks[track_id] == 10:
            GLib.idle_add(self.mark_remote_audio, track_id)
        if (self.continuity_baseline is not None and track_id == 1
                and self.remote_audio_tracks[1] >= self.continuity_baseline + 10):
            GLib.idle_add(self.mark_original_continued)
        return Gst.PadProbeReturn.OK

    def mark_remote_audio(self, track_id):
        if track_id not in self.remote_audio_complete:
            self.remote_audio_complete.add(track_id)
            count = len(self.remote_audio_complete)
            self.log("decoded 10 remote Opus buffers from distinct track #%d "
                     "(%d complete incoming tracks)" % (track_id, count))
            self.mark("remote-%d" % count)
            if count == 1:
                self.mark("remote-audio")  # Old 2-party probe compatibility.
        return GLib.SOURCE_REMOVE

    def mark_original_continued(self):
        if self.continuity_baseline is not None:
            self.continuity_baseline = None
            self.log("original remote Opus track continued after late join")
            self.mark("original-continued")
        return GLib.SOURCE_REMOVE

    def on_rtp(self, _pad, _info):
        if not self.audio_negotiated:
            # Keep caps/SDP negotiation intact; drop only early RTP buffers.
            return Gst.PadProbeReturn.DROP
        self.rtp_packets += 1
        if CONFERENCE and self.rtp_packets in (100, 500, 1500):
            # Verify that the live source keeps producing RTP after the
            # initial OnTrack callback. Local RTP alone is not SFU ingress.
            GLib.idle_add(self.log, "local Opus payloader produced %d RTP packets"
                          % self.rtp_packets)
        return Gst.PadProbeReturn.OK

    def on_bus(self, _bus, msg):
        if msg.type == Gst.MessageType.ERROR:
            err, debug = msg.parse_error()
            self.fail("GStreamer: %s (%s)" % (err.message, debug))

    def start_pipeline(self):
        for factory in ("webrtcbin", "audiotestsrc", "opusenc", "rtpopuspay"):
            if not Gst.ElementFactory.find(factory):
                raise RuntimeError("missing GStreamer plugin: " + factory)
        # Pion v4.2.6 rejects secondary application m-lines with port
        # zero despite bundle-only. max-compat keeps both RTP and SCTP
        # m-lines nonzero, including the first offer's application m-line.
        self.pipeline = Gst.Pipeline.new("matterleast-rtcd-poc")
        self.pc = Gst.ElementFactory.make("webrtcbin", "pc")
        if self.pc is None:
            raise RuntimeError("could not create webrtcbin")
        # MAX_COMPAT is supported on GStreamer 1.24 (BALANCED is not).
        # Unlike MAX_BUNDLE it advertises the subsequently added Opus m-line
        # with a nonzero port while retaining the BUNDLE group, so older Pion
        # versions do not mistake the media track for a rejected section.
        self.pc.set_property("bundle-policy", GstWebRTC.WebRTCBundlePolicy.MAX_COMPAT)
        self.pipeline.add(self.pc)
        self.pc.connect("on-negotiation-needed", self.on_negotiation_needed)
        self.pc.connect("pad-added", self.on_remote_pad)
        self.pc.connect("on-ice-candidate", self.on_local_ice)
        for name in ("connection-state", "ice-connection-state", "signaling-state"):
            if self.pc.find_property(name):
                self.pc.connect("notify::" + name, self.on_pc_property)
        bus = self.pipeline.get_bus()
        bus.add_signal_watch()
        bus.connect("message", self.on_bus)
        # GStreamer webrtcbin creates its SCTP transport when the first
        # data channel is registered. Follow the upstream sendrecv example:
        # configure the channel in READY, before transitioning to PLAYING.
        # This also makes the first negotiation include SCTP from the start.
        if self.pipeline.set_state(Gst.State.READY) == Gst.StateChangeReturn.FAILURE:
            raise RuntimeError("could not prepare WebRTC pipeline")
        self.dc = self.pc.emit("create-data-channel", "calls-dc", None)
        if self.dc is None:
            raise RuntimeError("webrtcbin cannot create calls-dc")
        self.dc.connect("on-open", self.on_data_channel_open)
        if SINGLE_STAGE:
            # The mainloop has not started: no asynchronous renegotiation
            # callback can race this setup. The initial SDP includes both
            # independent m-lines (max-compat), with no RTP sent before its
            # answer has been applied.
            self.start_audio()
            if self.error is not None:
                raise RuntimeError(self.error)
        if self.pipeline.set_state(Gst.State.PLAYING) == Gst.StateChangeReturn.FAILURE:
            raise RuntimeError("could not start WebRTC pipeline")
        # Explicitly trigger the initial offer. In the optional SCTP-only
        # mode, GStreamer 1.24 may never signal negotiation-needed without
        # an RTP sink; RTCD otherwise times out the join after 10 seconds.
        self.on_negotiation_needed(self.pc)
        self.log("started %s pipeline and calls-dc" %
                 ("single-stage Opus + SCTP" if SINGLE_STAGE else "SCTP-first"))

    def check(self):
        if self.error is not None:
            return GLib.SOURCE_REMOVE
        if CONFERENCE:
            # The original Alice↔Bob track must continue *after* Charlie's
            # arrival, not merely have produced enough buffers beforehand.
            if ((STATE_DIR / (PARTICIPANT + "-check-continuity")).exists()
                    and self.continuity_baseline is None
                    and not (STATE_DIR / (PARTICIPANT + "-original-continued")).exists()
                    and 1 in self.remote_audio_tracks):
                self.continuity_baseline = self.remote_audio_tracks[1]
                self.log("armed continuity check for original track at %d buffers" %
                         self.continuity_baseline)
                self.mark("continuity-armed")
            if (self.dc_open and self.answer_count >= 1 and not self.audio_added
                    and (STATE_DIR / (PARTICIPANT + "-publish")).exists()
                    and not self.offer_pending and not self.responding_to_offer
                    and not self.pending_server_offers
                    and self.pc.get_property("signaling-state")
                    == GstWebRTC.WebRTCSignalingState.STABLE):
                self.start_audio()
            if (self.audio_added and not self.audio_negotiated
                    and not self.offer_pending and not self.responding_to_offer
                    and not self.pending_server_offers):
                self.on_negotiation_needed(self.pc)
            if ((STATE_DIR / (PARTICIPANT + "-finish")).exists()
                    and self.audio_negotiated and self.dc_open
                    and len(self.remote_audio_complete) >= 2
                    and (PARTICIPANT == "charlie" or
                         (STATE_DIR / (PARTICIPANT + "-original-continued")).exists())):
                self.log("PASS conference %s: %d independent remote Opus tracks "
                         "(%d decoded buffers, %d server offers)" %
                         (PARTICIPANT, len(self.remote_audio_complete),
                          self.remote_audio_buffers, self.server_answers_sent))
                self.loop.quit()
                return GLib.SOURCE_REMOVE
        if (not CONFERENCE and self.answer_count >= (1 if SINGLE_STAGE else 2)
                and self.audio_negotiated and self.dc_open
                and self.rtp_packets >= self.rtp_at_open + 10):
            self.log("PASS: RTCD SDP, ICE/DTLS, DataChannel and audio RTP "
                     "(%d packets)" % self.rtp_packets)
            self.loop.quit()
            return GLib.SOURCE_REMOVE
        if time.monotonic() - self.started >= TIMEOUT_SECONDS:
            state = self.pc.get_property("ice-connection-state") if self.pc else "none"
            dc_state = self.dc.get_property("ready-state") if self.dc else "none"
            pc_state = self.pc.get_property("connection-state") if self.pc else "none"
            sctp = (self.pc.get_property("sctp-transport")
                    if self.pc and self.pc.find_property("sctp-transport") else None)
            sctp_state = sctp.get_property("state") if sctp and sctp.find_property("state") else "unknown"
            dtls = sctp.get_property("transport") if sctp and sctp.find_property("transport") else None
            dtls_state = dtls.get_property("state") if dtls and dtls.find_property("state") else "none"
            self.log("transport diagnostics: sctp=%r dtls=%r, dtls_state=%r" % (sctp, dtls, dtls_state))
            return self.fail(
                "timeout (offers=%d, answers=%d, audio_added=%s, "
                "audio_negotiated=%s, datachannel=%s, dc_state=%s, "
                "ice=%s, pc_state=%s, sctp_state=%s, rtp=%d)"
                % (self.offer_count, self.answer_count, self.audio_added,
                   self.audio_negotiated, self.dc_open, dc_state,
                   state, pc_state, sctp_state, self.rtp_packets)
            )
        return GLib.SOURCE_CONTINUE

    def run(self):
        try:
            self.connect_rtcd()
            reader = threading.Thread(target=self.receive_ws, daemon=True)
            reader.start()
            self.start_pipeline()
            GLib.timeout_add(200, self.check)
            self.loop.run()
        except Exception as exc:
            self.error = repr(exc)
            self.log("FAIL: " + self.error)
        finally:
            self.stop.set()
            if self.pipeline:
                self.pipeline.set_state(Gst.State.NULL)
            if self.ws:
                self.ws.close()
        return 1 if self.error else 0


if __name__ == "__main__":
    faulthandler.enable(all_threads=True)
    Gst.init(None)
    sys.exit(RTCDWebRTCPoC().run())
