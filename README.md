![Build status](https://github.com/Ri0n/matterleast/actions/workflows/build.yaml/badge.svg?branch=master)

# MatterLeast

MatterLeast is a native Mattermost desktop client built with Qt.

This repository is a maintained fork of [turok-m-a/mattermost-qt](https://github.com/turok-m-a/mattermost-qt), originally created by nuclear868. The application, executable and package are named `matterleast`. The Debian package explicitly conflicts with the old `mattermost-qt` package.

## Motivation

The official Mattermost desktop client is based on Electron and embeds the web application. MatterLeast aims to provide a lightweight native alternative with lower resource usage and conventional desktop UI behavior, including native widgets, context menus and thread windows.

## Highlights

- Channels, direct messages and group messages.
- Sending, receiving, editing and deleting messages.
- Threads in separate windows, including follow/unfollow support and restoration of the previous reading position.
- File attachments, image previews and opening/downloading non-image attachments.
- Emoji and reactions, including adding a reaction by clicking an existing reaction chip.
- Markdown rendering, quoted messages and fenced code blocks with syntax highlighting.
- Pinned posts with direct `Go to message` navigation.
- Mattermost-style server-backed sidebar categories, Favorites/custom categories, mute state and drag/drop ordering.
- `Channels`, `Following` and `Attention` sidebar views, including unread filtering and followed-thread/mention handling.
- Direct-message search and lazy server-side user discovery.
- Presence/status indicators and profile dialogs with full-resolution avatars.
- Desktop notifications with message/thread targets on supported Linux desktops, with tray notifications as a fallback.
- Reliable WebSocket reconnect/resume using Mattermost connection and sequence state, avoiding the old full-history refresh after a successful resume.
- Lazy loading of channels, user profiles, avatars and member lists instead of downloading the complete server directory at startup.
- Sparse/windowed channel and thread histories. Long conversations keep only a bounded number of materialized message widgets while older/newer regions are loaded on demand.
- Viewport anchoring and sticky-bottom behavior designed to keep the reading position stable while history, images and message layouts change.

The client is under active development and does not yet provide complete Mattermost feature parity. Voice/video calls and some less common server/plugin features are not implemented.

## Platforms and CI

The project uses C++17 and CMake 3.16 or newer.

Current CI covers:

- Qt 6.10 on Ubuntu;
- Qt 5 on Ubuntu 22.04;
- Qt 5 on Fedora;
- Qt 6.10 with MSVC on Windows;
- Ubuntu 26.04 package builds.

Unit tests are run in the CI matrix. Compiler warnings can be promoted to errors with `-DWARNINGS_AS_ERRORS=ON`.

A separate manually triggered [Packages workflow](https://github.com/Ri0n/matterleast/actions/workflows/packages.yml) builds:

- DEB packages for Ubuntu 22.04 / Qt 5;
- DEB packages for Ubuntu 24.04 / Qt 6;
- DEB packages for Ubuntu 26.04 / Qt 6;
- a Windows x64 NSIS installer using Qt 6 and MSVC.

## Build requirements

Required Qt modules:

- Widgets
- Network
- WebSockets

Qt 6 is preferred by default. Qt 5 is still supported and can be selected explicitly with `-DQT_6=OFF`.

On Linux, Qt DBus is optional and enables native freedesktop notifications when available.

### Ubuntu / Debian

For Qt 6:

```bash
sudo apt install build-essential cmake qt6-base-dev qt6-websockets-dev
```

For Qt 5:

```bash
sudo apt install build-essential cmake qtbase5-dev libqt5websockets5-dev
```

## Build instructions

It is recommended to use an out-of-tree build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

To force a Qt 5 build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DQT_6=OFF
cmake --build build --parallel
```

Tests can be enabled explicitly with:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

### Windows

Windows is actively built in CI with Qt 6.10 and MSVC. A normal Qt/CMake build works with the same source tree; the packaging workflow additionally uses Qt deployment support and CPack/NSIS to produce a self-contained installer.

## Running

From the build directory, start:

```bash
./matterleast
```

A login dialog appears on first start. After successful login, the server
URL and username are saved in application settings. When built with
QtKeychain support, the session token is saved asynchronously in the platform
credential store (Windows Credential Manager, macOS Keychain, Linux Secret
Service/KWallet). The password itself is never saved.

CMake detects QtKeychain automatically: `MATTERLEAST_USE_KEYCHAIN` defaults to
`ON` if the Qt5/Qt6 matching development package is installed, otherwise
`OFF`. Explicit `-DMATTERLEAST_USE_KEYCHAIN=ON` overrides the detection and
can build the pinned bundled library; explicit `OFF` always disables it. The
automatic `OFF` case does not download any dependencies and does not persist
login sessions. Official release packaging explicitly enables keychain support
to keep autologin working across restarts.

For upgrades, the old plaintext settings token is read only to resume the
existing session. After a successful secure write it is cleared from
application settings. An unavailable or locked keyring never triggers a
plaintext fallback for new tokens. Existing settings copies, backups or
filesystem snapshots are not securely wiped by this migration.

## Locked crypto dependencies

`dependencies.lock.json` is the single source of the QCA and QtKeychain
release tags, full Git commits and minimum system-library versions. The same
lock structure is used by Iris. To update a dependency, verify the upstream
release tag and its commit, change the lock, then run CI; do not duplicate pins
in CMake or GitHub Actions.

For local message encryption, MatterLeast supports system QCA 3.0.10+ or
QCA 2.3.4+ with its OpenSSL crypto provider. CMake selects the matching Qt
major version in this order: **installed QCA 3 → installed QCA 2 →
locked QCA 3.0.12**, downloaded only if no compatible system package exists.
The shared AES-256-GCM envelope is compatible across QCA 2 and QCA 3.

Windows and AppImage jobs explicitly request reproducible bundled QCA 3.
**Bundled QCA 3 is fully static**: both the QCA core and `qca-ossl` are
static libraries linked into the application, with the provider registered by
`Q_IMPORT_PLUGIN(qca_ossl)`. No QCA DLL, shared library or plugin file needs
to be deployed. Native DEB and RPM jobs instead use QCA 2 development files
and the matching dynamically loaded OpenSSL provider. For a forced bundled
build, use:

```sh
cmake -S . -B build -DMATTERLEAST_BUNDLED_QCA=ON
```

In bundled mode CMake downloads QCA 3 and statically links its OpenSSL
(`qca-ossl`) provider. Building that provider needs OpenSSL development
headers and libraries (`libssl-dev` on Debian/Ubuntu, `openssl-devel`
on Fedora); these packages are **not needed for MatterLeast builds using a
compatible system QCA and its prebuilt provider**. Native distro packages
depend on their QCA runtime library and dynamically loaded OpenSSL provider.
Corporate builds using `MATTERLEAST_PLUGIN_VERIFY_KEY_FILE` additionally
require OpenSSL development headers: Ed25519 plugin signature verification
still calls OpenSSL EVP directly, since the QCA 2 API does not expose the
Ed25519 signature scheme. The build-time `openssl` command is also required
when signing bundled native plugins. HTTPS uses QtNetwork's TLS backend,
not QCA::TLS. Runtime message encryption fails closed if the QCA provider
or platform keyring is unavailable.

For QtKeychain, matching system development files are preferred; when
explicitly enabled without them, CMake fetches the exact pinned version
(currently 0.17.0). This leaves supported Linux distributions free to use
their system QtKeychain implementation.

## Security considerations

Cached post payloads are encrypted at rest using QCA AES-256-GCM after
compression. Locally saved draft message text is also encrypted. SQLite
topology/index fields and non-message draft metadata remain plaintext.
A distinct random device encryption key is held in the platform keyring,
not derived from the Mattermost session token. When the keyring or QCA
AES-GCM provider is unavailable, local message persistence is disabled
rather than falling back to plaintext. Legacy plaintext post-cache data is
discarded; legacy draft JSON is migrated once the keyring can be unlocked.
Already-created backups or snapshots cannot be securely wiped. This is
**at-rest encryption**, not end-to-end encryption.

A corporate build can enforce signed-only native plugins by specifying its
Ed25519 PUBLIC verification key as a PEM file:

```bash
cmake -S . -B build \
  -DMATTERLEAST_PLUGIN_VERIFY_KEY_FILE=/secure/build/company-public.pem
```

Such a build rejects unsigned or invalidly signed MatterLeast plugins before
loading their code. It still supports corporate plugins and signed bundled
plugins. The private signing key must never be stored in the application or
source repository. In a separately secured release environment, set
`MATTERLEAST_PLUGIN_SIGNING_KEY_FILE` to sign bundled plugins during the
build. Each native library receives a raw Ed25519 sidecar `<library>.sig`
covering its complete linked binary; do not modify the library after signing.

To sign externally built plugins with OpenSSL 3:

```bash
openssl pkeyutl -sign -rawin -inkey company-private.pem \
  -in company-plugin.so -out company-plugin.so.sig
```

Alternatively, `-DMATTERLEAST_ENABLE_NATIVE_PLUGINS=OFF` disables all
MatterLeast native plugins, including built-in ones. Normal builds can also
disable MatterLeast native plugins at runtime with
`--disable-native-plugins` or `MATTERLEAST_DISABLE_NATIVE_PLUGINS=1`.
Runtime switches are user-controlled and not an administrator-enforced policy.
Plugin signing does not sandbox trusted plugins or cover unrelated Qt plugins.

## Contributing

Bug reports and pull requests are welcome. MatterLeast is maintained as a lightweight native Mattermost client, so changes that improve compatibility, reliability, resource usage and normal desktop usability are especially useful.
