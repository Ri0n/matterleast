# Calls provider architecture (phase 1)

## Boundary

MatterLeast's ordinary chat executable, Qt Widgets UI, Backend, and installed
native-plugin SDK do **not** depend on GStreamer. The Calls capability is
implemented by an optional native MatterLeast plugin loaded through the
existing PluginManager/PluginHost and QPluginLoader infrastructure.

The public `plugins/api` interfaces are intentionally independent of media
engines and the internal `Backend`:

- `CallsProviderInterface` (IID `CallsProvider/1.0`): start, join, leave, mute,
  apply server events, connection changes, and a by-value `CallsSnapshot`.
- `CallsHostAccessor` (IID `CallsHostAccessor/1.0`): injected before
  `PluginInterface::enable()`. The base plugin IID remains unchanged.
- `CallsHostInterface` (IID `CallsHost/1.0`): logged-in identity, scoped
  authenticated Calls REST requests, sends to the existing authenticated
  sequenced Mattermost WebSocket, and state-change notifications.
- `CallsTypes.h`: state and participant descriptors using only QtCore types.

Clients use `PluginManager::callsAvailable()`, `callsSnapshot()`, and its
call actions. **Never retain** an interface pointer across a native plugin
disable/unload. The manager checks the enabled provider on each operation.

### Session ownership

A real provider may be discovered before login; it must not require a
credential or create media on `enable()`. The call may start only when the
Backend is authenticated. All Mattermost API authentication, cookies, TLS
configuration, reconnect handling, and network transport remain in the core.
The Calls host accepts only supported HTTP methods on official Calls plugin
paths below `plugins/com.mattermost.calls/` at the server root. An HTTP completion is
discarded when its QObject request context is destroyed. Importantly, its
plugin-owned `std::function` target is cleared synchronously before the
module unloads; merely testing a `QPointer` on a later response is not enough,
because the deferred callback's destructor could otherwise execute unloaded
plugin code. The provider receives forwarded Calls-specific
`custom_com.mattermost.calls_*` WebSocket events and backend connection
changes, not raw credentials. Outgoing Calls actions use the same
authenticated WebSocket and the connector's existing sequence counter;
the plugin must not create an independent main Mattermost socket.

The provider owns the call state machine and must publish updated snapshots
through `CallsHostInterface::providerChanged()`. In particular, its
`PluginInterface::disable()` **must refuse** while it owns an active call,
a pending asynchronous operation, media callbacks, or UI objects that have not
been disconnected. `PluginHost::disableAndUnload()` respects such a refusal.

A real Calls implementation needs asynchronous states (Connecting, Connected,
Leaving, Failed) driven by server acknowledgements; the fake test provider
completes transitions synchronously, deliberately without speaking to a server.
This stage adds neither active Calls UI nor a production Calls client.

### Media engine

The future `matterleast-calls-gstreamer` plugin will own the Calls protocol,
per-call participant model, and its internal `IMediaEngine`. The first engine
will use GStreamer's native C API from C++, with all pipeline operations on an
explicit GLib context and results delivered to Qt through queued events. No
GStreamer/GLib/Pion types cross the public Calls plugin ABI.

A Mattermost call uses one local `webrtcbin` peer connection connected to
RTCD's SFU and independently identified inbound tracks for each remote
participant. The validated isolated PoC with **three peers and a late third
join** is retained in draft PR #340. The provider must use the supported
Mattermost Calls REST/WebSocket protocol, **not** the PoC's internal RTCD
MessagePack control WebSocket.

An out-of-process media worker remains a possible hardening step if process
isolation is needed; it can be introduced behind `IMediaEngine` without
changing the Calls API or application-side UI.

## Debian / Ubuntu release design

Target **Ubuntu 24.04 LTS, distro GStreamer 1.24** first. GStreamer is optional
only if the distribution packages it separately:

- `matterleast`: main app, no `libgstreamer` dependency.
- `matterleast-calls-gstreamer`: native plugin and `Depends` on the matching
  MatterLeast build, `libgstreamer`, libnice, and GStreamer RTP/Opus/DTLS/
  SCTP/audio plugins. Use explicit runtime plugin dependencies in addition to
  `dpkg-shlibdeps` (which sees shared libraries but not registry-loaded
  plugins). Main package can `Suggests` the optional one.
- The plugin module lives in the **existing** multiarch MatterLeast plugin
  directory. Do not ship it inside the main DEB: the current
  `CPACK_DEBIAN_PACKAGE_SHLIBDEPS=ON` would otherwise make GStreamer
  mandatory for all installations.
- Keep the two-package setup verifiable in CI: install `matterleast` alone,
  confirm it starts without GStreamer, install the matching Calls package,
  verify the plugin and media capabilities, then disable the plugin and
  confirm the chat application remains fully functional.

Package splitting and real Calls media implementation belong to a follow-up
PR once a true GStreamer target exists. Phase 1 does **not** add a redundant
or empty package to the release. The fake Calls provider is compiled for tests
only, never installed in production.

## Phase 1 verification

`plugin-host-test` builds and loads a fake Calls provider with no multimedia
libraries and tests missing host, pre-login discovery, authenticated join,
mute, remote events, state notifications, refusal to unload a connected call,
leave, disable/unload, re-enable, connection-failure state, and cleanup.

The existing plugin SDK external smoke build, signed-plugin policies and
normal Qt 5/Qt 6 CI must remain green. No existing plugin IID is modified.

## Whiteboard transport (design only)

Psi's collaborative drawing UI and document/operation model should remain
separate from XMPP/Jingle/SXE. MatterLeast can reuse the interaction and
presentation concepts, but must inject a transport adapter; merely adding
a GStreamer data channel is **not** a peer broadcast mechanism.

The official Mattermost RTCD service uses its existing `calls-dc` for RTC
signaling/metrics, with an explicit message-type decoder and a server-side
handler per data channel. The SFU does not forward arbitrary custom SCTP
messages to other call participants. Creating a second channel on the same
peer connection would also invoke its server-side data-channel handler;
it is not a supported, independent whiteboard lane.
Sources: https://github.com/mattermost/rtcd/blob/master/service/rtc/dc.go
and https://github.com/mattermost/rtcd/blob/master/service/rtc/dc/msg.go.

A practical first transport is an **optional, authenticated Mattermost
server-side extension**, relaying whiteboard operations over the established
Mattermost WebSocket mechanism. Only authorized participants in the given
call/channel may publish or subscribe. A late joiner should receive a
versioned document snapshot and then an ordered operation tail. Keep
cursor/pointer moves ephemeral and throttled; persist semantic drawing
operations (shapes, paths, deletes, undo metadata) with sequence/operation
IDs and bounded replay. Do not encode every mouse-move as a chat post.
Neither the current Calls provider nor RTCD should be modified until the
server capability and wire format are agreed.

The drawing widget should depend only on an interface such as
`IWhiteboardTransport` (send operation, receive operation, request snapshot,
connection state); a WebRTC DataChannel transport can be added later if
Calls/RTCD explicitly supports routing those messages to all participants.
Do not label server-relayed whiteboards as end-to-end encrypted.
