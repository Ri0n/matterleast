# RTCD ↔ GStreamer `webrtcbin` PoC

This isolated CI experiment tests real GStreamer 1.24 interoperability with
`mattermost/rtcd:v1.2.6`, including a **three-participant audio conference with a late join**.
It does not add GStreamer to MatterLeast's production dependencies.

## Transport and compatibility

The test authenticates with RTCD's **internal** MessagePack WebSocket
interface, using temporary self-registration only in the disposable CI
container. It uses two independent WebRTC clients and RTCD sessions,
without a running Mattermost application server.

Each GStreamer peer negotiates **Opus and `calls-dc` SCTP DataChannel in
a single initial offer** with `bundle-policy=max-compat`. This gives both
audio and application m-lines nonzero ports. RTCD v1.2.6's Pion v4.2.6
does not initialize SCTP for a secondary `m=application 0` even if the
SDP correctly marks it `a=bundle-only`.

RTCD v1.2.6 also compares the incoming audio codec's MIME type with
`"audio/opus"` **case-sensitively**. Unmodified GStreamer 1.24 produces
`"audio/OPUS"`; Pion accepts the packets, but RTCD's SFU fails to
recognize the media track and cannot forward it. For this PoC only,
the outbound SDP `a=rtpmap:111 OPUS/48000/2` is sent as
`a=rtpmap:111 opus/48000/2`, while GStreamer's *local* description
and RTP caps remain unchanged. If an SDP answer carries the lowercase
name, the copy applied to GStreamer is normalized to uppercase. These
are case-only changes permitted by SDP/RTP codec-name semantics;
the RTP payload, ICE, DTLS, SCTP and media formats are not modified.
**RTCD should ultimately fix its case-sensitive codec comparison.**

Incoming `webrtcbin` pads are linked to the Opus decoder **synchronously**
inside `pad-added`. Deferring that link to a GLib idle callback can let the
first RTP packet reach an unlinked pad and propagate `GST_FLOW_NOT_LINKED`
back to `nicesrc`, terminating the receive stream.

The previous two-stage SCTP-first negotiation remains available for
comparison via `RTCD_SINGLE_STAGE=0`, but the primary PoC uses the
single-offer mode.

## CI coverage

The workflow on Ubuntu 24.04 downloads GStreamer/libnice and runs the
authentic RTCD Docker image on the runner's host network, avoiding NAT
ambiguity. To avoid slow regional Ubuntu archives, it selects a known-fast
public mirror and caches the downloaded GStreamer `.deb` packages.

The standalone test checks authenticated RTC join, SDP and ICE exchange,
connected DTLS/SCTP, open DataChannel, continuing local Opus RTP and an
RTCD log confirming actual RTP ingress.

`rtcd_conference_poc.py` runs **three independent GStreamer processes**,
each with its own user/session IDs but sharing a single RTCD conference.
It verifies the progression in separate, state-driven phases:

1. Alice publishes Opus; RTCD confirms ingress.
2. Bob joins and exchanges audio with Alice. Both must already have
   decoded **10+ incoming audio buffers** before the third user starts.
3. **Charlie joins later**, with a new WebSocket, peer connection, and
   sender. RTCD initiates further SDP offers and all peers answer them.
4. **Each of Alice, Bob and Charlie decodes 10+ PCM buffers on each
   of two distinct incoming RTP pads**, through `rtpopusdepay ! opusdec`.
   Aggregate decoded-packet counts do not satisfy this assertion.
5. After Charlie is active, the test establishes a new buffer-count
   baseline for Alice's original Bob stream and Bob's original Alice
   stream. Both must deliver **10 additional buffers** to prove that
   the existing media survives late-join renegotiation.
6. RTCD must confirm inbound Opus for all three sessions; all three
   processes must finish successfully.

This exercises all **six directed audio paths** through the SFU, plus
the continuity of pre-existing sessions. It uses synthetic audio;
no microphone, camera or Mattermost account is needed.

The three-participant conference first passed in
[GitHub Actions run #44345755305](https://github.com/Ri0n/matterleast/actions/runs/44345755305).

Run **RTCD / GStreamer WebRTC PoC** manually from GitHub Actions
(**Actions → RTCD / GStreamer WebRTC PoC → Run workflow**). The workflow is
intentionally **manual-only**: changing these files or pushing to a PR does
not launch Docker RTCD or the expensive GStreamer installation.

## Scope

This tests transport interoperability, not Mattermost Calls client
integration. The separate native MatterLeast Calls plugin was merged in
[#343](https://github.com/Ri0n/matterleast/pull/343), and uses the official
Calls REST/WebSocket protocol instead of this PoC's RTCD internal interface.
These tests cannot establish production end-to-end Calls compatibility;
video, screen sharing and full production media lifecycle coverage remain
separate concerns.
