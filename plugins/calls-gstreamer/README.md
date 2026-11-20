# MatterLeast Calls — optional GStreamer plugin

This Linux native extension is experimental. It provides an initial audio
integration with the official Mattermost Calls plugin, using **system
GStreamer 1.24+**, a single `webrtcbin` PeerConnection, Opus RTP media, and
the existing authenticated MatterLeast WebSocket connection. It does not
modify the chat application's mandatory runtime dependencies.

## Ubuntu 24.04

The Ubuntu 24.04 CI compiles against stock system GStreamer 1.24.2. It
produces two separate packages:

- `matterleast_<version>_amd64.deb` — ordinary desktop client, **no
  GStreamer dependency**.
- `matterleast-calls-gstreamer_<version>_amd64.deb` — optional extension.
  Requires the exactly matching `matterleast` package version, GStreamer
  shared libraries and the audio/Opus/WebRTC registry plugins.

To install the optional capability from a matching build:

```shell
sudo apt install ./matterleast_<version>_amd64.deb
sudo apt install ./matterleast-calls-gstreamer_<version>_amd64.deb
```

The plugin installs to the distro multiarch native-plugin directory
(`/usr/lib/x86_64-linux-gnu/matterleast/plugins/` on amd64), alongside
ordinary MatterLeast plugins. On application startup it is discovered by the
existing PluginManager. No independent Mattermost login is required.

Build locally with `-DMATTERLEAST_BUILD_CALLS_GSTREAMER=ON` to enable this
optional target. The CMake option defaults to **OFF**. The GStreamer backend
is compiled separately and does not expose `Gst*` or GLib pointers through
the public Qt plugin API.

## Call window

When the Calls native plugin is available, a phone button appears directly
beside the conversation-actions menu in DM and GM headers (not in channels
or threads). It starts/joins the audio call; during a call in the same
conversation it becomes a leave/cancel action. Other DM/GM call buttons are
disabled while this client owns a different call. The button is removed from
layout when the optional plugin is disabled.


The optional module provides a standalone, modeless Qt Widgets audio-call
window with a resizing participant stage and a single compact control dock.
The window opens on call join/start; its title uses the conversation name,
and the end/cancel button shows elapsed duration after connection. The mic
control mirrors the actual provider snapshot and changing its checked state
programmatically does not send duplicate mute commands. Closing the native
window leaves the call.

The layout is informed by Psi's `CallView` / `CallStage` conventions, but
**the source is an independent LGPL-3.0-or-later implementation**, not a
verbatim copy of Psi's GPL-2.0-or-later widgets. There is no Jingle or
PsiMedia linkage, and the public Calls plugin ABI remains QtCore-only.
The current stage shows participants as stable audio tiles, preserving each
tile across incremental joins and leaves. The stage reflows between one,
two and three-or-more columns on resize without recreating existing tiles;
a short connection-status header, participant count, microphone states and
clear empty/error state remain visible even before media joins. Palette-aware
controls include a working mute toggle (also Ctrl+M inside the call window)
and an end-call button with an elapsed timer. The local participant is
identified as “You” when the provider knows the local user ID. Opaque IDs are shown as initials with the complete ID in the tooltip;
resolving friendly display names still requires a host lookup capability.
Camera, screen-sharing, media-device selectors and security verification are
intentionally not shown until implemented; displaying a padlock as if an SFU
call were verified end-to-end encrypted would be misleading.

## Tests and current limitations

Successful Ubuntu 24.04 CI includes:

- Native module loading with `QPluginLoader`, Calls host/API injection and
  authenticated join request via the main Mattermost WebSocket.
- An **actual GStreamer** Opus+SCTP local SDP offer after a server join ack,
  teardown, and refusal to unload the plugin during a live media session.
- Correct MessagePack binary/zlib SDP framing; deterministic parsing of
  existing participants after a late join through `call_state`.
- Independent Qt window regression test: stable tiles with a late third
  participant, microphone signal semantics, call duration, native close,
  error messages and offscreen dark/light-palette-safe widgets.
- Generation and installation of two separate DEBs, runtime ELF deps, and
  loading the **installed** native module in a smoke test.

The earlier separate [three-participant RTCD transport
PoC](https://github.com/Ri0n/matterleast/pull/340) verified SFU forwarding
and late-join media, but it used RTCD's **internal protocol**. The native
plugin uses the **official Mattermost Calls protocol** and still requires
an end-to-end test against a real Mattermost server with Calls enabled
before it can be considered production-ready. Do not treat a successful
local SDP offer as proof of ICE, DTLS, RTP exchange with a server.

Present scope is audio. TURN configuration, WebSocket reconnection behavior,
glare handling, device selection, remote track attribution to users, video,
screen sharing and a full-featured call UI need further validation and work.
