# HTTP client profile and transport

MatterLeast intentionally keeps one HTTP client profile for all requests sent
through `HTTPConnector`. This avoids Qt-version drift and keeps ordinary API,
plugin and multipart traffic consistent.

## Reference client

The current compatibility reference is the supported official Mattermost
Desktop v6.3.0 release:

- Mattermost Desktop 6.3.0
- Electron 43.0.0
- Chromium 150.0.7871.46

The profile is centralized in `NetworkRequest`; endpoint code should not grow
its own generic browser/client headers.

## Multi-server HTTP identity boundary

`NetworkSessionContext` is the new per-account owner for normalized server
host, session token, returned session cookies and CSRF. Its request factory
and policy methods do not depend on global state. Two contexts can be used
interleaved in one GUI thread without exchanging authentication headers;
authentication headers are only added to the matching scheme, host and port.

`NetworkRequest` now offers constructors taking an explicit
`NetworkSessionContext`. Its old static `setHost()`, `setToken()`,
`clearToken()`, `getToken()`, `host()`, cookie update and desktop-profile
methods intentionally remain a **legacy singleton facade** for the current
one-account runtime, preserving existing behavior. Do not use that facade
in new multi-account code. The production application cannot yet present or
manage simultaneous full-featured Backends; the compatibility facade remains
until its UI and service callers have been migrated.

`HTTPConnector` can now be constructed with a non-owning
`NetworkSessionContext*`. In that mode, it applies the owner's credentials
at **enqueue** time, replacing any accidentally inherited `Cookie`,
`X-CSRF-Token` or `Authorization` headers; this also prevents a stale
legacy request from sending the global token. Returned cookies are admitted
only when the actual response origin matches the session context. The old
default constructor keeps its existing behavior.

An explicitly bound connector uses a distinct HTTP disk cache directory,
derived from a SHA-256 digest of its configured server base URL and stable
account identity (currently the login name). Raw server URLs, usernames and
tokens never occur in cache directory names. On missing identity or filesystem
failure, caching is disabled rather than falling back to another account's
directory. The old single-account cache path remains supported by the legacy
connector. Reset and network-route restart preserve the bound cache partition.
The configured size limit is currently per account; the future session manager
can divide an overall cache budget among connected accounts.
This isolation does not authorize callers to reuse a wrong URL: independent
sessions must still construct requests against their own context.

`Backend` now owns its own `NetworkSessionContext` for its entire
lifetime, and constructs its `HTTPConnector` bound to that context. Its
authenticated login, retry and logout requests, post-login user preferences,
and websocket URL/token are sourced directly from its instance context.
Two independent `Backend` instances can authenticate to separate local
Mattermost API fixtures at once; regression tests also check that cancelling
one in-flight login does not cancel another.

**Migration boundary:** every API request constructed directly by
`Backend` (including `BackendChannelMembers`, `BackendOwnStatus`,
`BackendChannelTopic`, `BackendPostCreate`, and `BackendTeamDirectory`)
now gets an explicit `Backend::networkSession()` source for its URL and
authorization. Async membership callbacks verify the same Backend session
origin, not the most recent global login. Independent Backend integration
tests assert that ordinary post-login profile fetches hit two different
local Mattermost servers even when the global legacy URL points at only one.

The current single-server login **still writes** the legacy
`NetworkRequest` host/token for yet-unmigrated service and UI request
producers. They must be moved to their owning Backend context before the
facade can be removed or multi-server operation enabled in the UI.
Standalone test Backends that intentionally skip login now explicitly
initialize their own `networkSession()` origin; the Backend no longer
silently relies on the global host for its own HTTP methods.
No new current-account global, connection manager or UI switch is
introduced by this step.

Before enabling production multi-session operation, route *all* API request constructors,
response `Set-Cookie` handling, websocket login URLs and login/retry/logout
through their owning session context. Revisit services that construct absolute
URLs, attachment caches, and other global state (reaction usage,
server/plugin configuration, etc.) before allowing a second live Backend. Forbid an
implicit-current-account fallback in these paths. Keep context lifetime longer
than queued requests, and guard async replies by their owning session generation.

## Account-owned service transports

`Backend::httpTransport()` supplies the existing account-owned connector
for routine attachment fetching, post creation and post repository requests.
They share the Backend's connection pool, scheduling queue, error reporting
and account-partitioned HTTP disk cache rather than creating duplicate
`QNetworkAccessManager` objects and competing cache directories.

Services whose request lifecycle or expected 404 responses must remain
independent (e.g. thread follow lookups, draft synchronization, channel
reference resolution, emoji discovery, user profile fetches, sidebar requests,
and plugin bundle discovery) retain separate `HTTPConnector` instances
**bound to their owner's `Backend::networkSession()`**. They cannot send
requests to the last-selected account's URL, and cannot send its credentials.
Those secondary connectors intentionally have no additional disk cache.
For efficiency, account-bound connectors create their Qt network manager
lazily on the first request, and reset/reconnect does not allocate another
manager while the service is idle.

Download-to-file streaming retains a separate Qt network manager to avoid
buffering whole files into memory. Its requests use the owning Backend's
`NetworkSessionContext`; it also installs the same no-replay cookie jar
policy as account-bound API connectors. A streamed response cannot silently
populate an independent Qt cookie jar with credentials that outlive logout
or account changes. This is covered by two sequential streaming downloads
from each of two independently authenticated Backends; the local API fixture
tries to set a cookie on the first stream and verifies it is not sent back.
Caller-owned files and API authentication remain separate concerns.

End-of-session cancellation now uses
`HTTPConnector::resetSessionTransports(&backend.networkSession())` to reset
only connectors bound to that exact context, including isolated service
connectors and the shared Backend transport. One Backend's logout cannot
cancel another server's pending requests. `Backend` also tears down its
QObject child services during its own destructor while the account session
and Storage members still exist, preventing references to dead account
state during service destruction.

No process-global current-account fallback, additional timers or model
resets are introduced. Integration fixtures authenticate two Backends to
two local server origins, verify account-correct attachments and channel
references, and prove that reset of one account cancels only its own HTTP
callbacks (even with multiple service connectors).

## Account-owned navigation, links and UI state

UI navigation no longer interprets Mattermost URLs relative to the last
process-wide login. `AppNavigationService` classifies absolute and relative
links against its owning Backend's `NetworkSessionContext::host()`.
Cold-post resolution reuses `Backend::httpTransport()`, fetching post,
channel and team records in the correct server's Storage without allocating
a short-lived `QNetworkAccessManager` for each link.

`PostPermalinkService` constructs copied `/_redirect/pl/` links from an
explicit Backend, and its canonical-SiteURL lookup uses a service connector
bound to that Backend. This keeps official native forward previews anchored
to the correct server even when the two accounts share post IDs. Channel
web links, KTalk plugin requests, and navigation-session persistence keys
also use the owning Backend's server identity. Session-key hashing preserves
the previous `host + unit separator + user ID` layout, so existing
single-account saved tabs remain readable.

Auto-joining a stored public channel reuses its Backend HTTP transport
instead of creating an ephemeral connector per click; an asynchronous reply
is ignored if the sidebar now belongs to a different Backend.

The opt-in AI MCP server now has exactly one QObject-owned instance per
Backend rather than a process-wide singleton. Its short-lived capability
tokens are matched to that Backend's login user and HTTP origin, and are
revoked upon `Backend::sessionEnding()` even when the same user later logs
in again to the same origin. This prevents an AI window in one account from
reusing another account's local MCP endpoint.

These changes leave the old process-global facade available only for
remaining unmigrated consumers, not as an implicit fallback in new UI code.

## Per-account local post deletion

`LocalPostDeleteTracker` is owned by `Storage`, never a process-global set.
The primary `HTTPConnector` receives a non-owning pointer to that account's
tracker. A DELETE request marks an origin in the current account; its matching
WebSocket event consumes the same account's marker via `BackendChannel`.
Transport reset, failed DELETE and Storage reset clear only the owning
account's markers. Standalone HTTP connectors without a Storage tracker do
not record local post-deletion origins.

The tracker is an in-memory one-shot identity set, with no timers or
persistence; logout/retry does not mutate any other account. A test uses the
same channel and post ID in two independent Storage instances to verify that
only the originating account removes its local timeline row, while the other
treats the event as a remote deletion.

## Common request profile

Every request sent through `HTTPConnector` receives the common profile even
when its caller supplied a plain `QNetworkRequest`:

- official-desktop-shaped `User-Agent`, platform-specific at the OS token;
- `X-Requested-With: XMLHttpRequest`;
- `Accept: */*` unless the endpoint overrides it (file upload uses
  `application/json`);
- `Accept-Language` from the Qt/system UI language;
- HTTP/2 explicitly allowed on Qt 5 and Qt 6;
- redirects limited to `NoLessSafeRedirectPolicy`;
- TLS 1.2 or later, with TLS 1.3 still negotiated when supported.

Qt 5.15 defaults `Http2AllowedAttribute` to false while Qt 6 defaults it to
true. Never rely on that default: protocol negotiation must be identical across
supported Qt versions.

MatterLeast does not impose its own process-global concurrent-request ceiling
in front of Qt. `QNetworkAccessManager` already applies protocol-specific
backpressure: HTTP/1.1 is queued per host/port, while HTTP/2 multiplexes over
the negotiated connection and honors the peer's stream limits. A second global
cap would unnecessarily serialize unrelated requests and hide available HTTP/2
capacity.

## Session cookies and CSRF

Mattermost Desktop's web application uses browser cookies and deliberately
disables the generic Bearer Authorization header so normal CSRF validation
remains active.

MatterLeast mirrors that model for same-origin requests:

- `Set-Cookie` responses update the backend-scoped session cookie snapshot;
- `MMAUTHTOKEN`, `MMUSERID`, `MMCSRF` and any other returned session
  cookies are carried forward in the `Cookie` header;
- non-GET requests include `X-CSRF-Token` when an `MMCSRF` cookie exists;
- token-only logins that did not pass through `/users/login` retain
  `MMAUTHTOKEN=<token>` as a compatibility fallback;
- authentication cookies and CSRF headers are never copied to another origin.

Do not add `Authorization: Bearer` alongside browser-session cookies merely for
similarity. The official Channels web application explicitly disables that
header for its cookie-authenticated request path.

## Headers deliberately not synthesized

Do not fabricate Chromium-only provenance/fingerprinting headers such as
`Sec-Fetch-*`, `sec-ch-ua*`, `Origin` or `Referer` globally.

Those headers are generated by a browser from a real document/navigation
context, and their consistency is checked against the browser networking/TLS
stack by some intermediaries. A Qt request with hand-written browser provenance
can be less coherent than a native request.

Likewise, QNetworkAccessManager/OpenSSL or Schannel cannot reproduce Chromium's
TLS fingerprint simply by changing HTTP headers. If a deployment requires an
actual Chromium transport fingerprint, that is a different networking backend
decision, not another header in `NetworkRequest`.

## Endpoint-specific headers

Endpoint code may add headers that the official client adds for that operation,
for example `Connection-Id` on file upload. The centralized profile preserves
such explicit values and fills only missing common headers.

## Diagnostics

Use the `mattermost.upload` trace for attachment transport comparison. It
records Qt/SSL versions, negotiated TLS/cipher, whether HTTP/2 was used, and
sanitized request/response headers. Authentication and CSRF values are redacted.

## Offline startup and saved-session recovery

The login dialog never considers a persisted token sufficient proof of
authentication. `/users/me` must return a successful HTTP response containing
a nonempty user ID before `Backend::loginSuccess()` mutates Storage, starts
WebSocket or allows the main window to be constructed.

Saved-token auto-login retries transient DNS/transport failures and temporary
HTTP 408/429/5xx responses every three seconds while keeping the login dialog
open. Invalid/expired credentials (401/403 and other permanent client errors)
stop automatic attempts and return control to manual login. A manual login
cancels any pending automatic retry. This prevents partial main-window
initialization that cannot recover by merely establishing WebSocket later.

## Reply lifetime during HTTP transport restart

When reconnecting after a network route change, the HTTP transport replaces
its network manager. The outstanding GET requests are replayed on the new
transport, while mutations are not replayed automatically because they may
already have been applied by the server.

Calling `QNetworkReply::abort()` can invoke callbacks synchronously.
Those callbacks can enter nested event loops or destroy other outstanding
replies. Therefore restart iterates `QPointer`-guarded replies, checks that
each reply is still active before aborting it, and tracks destruction to
prevent stale pointers and incorrect active-request accounting.
