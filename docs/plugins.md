# Plugin architecture

MatterLeast native plugins use a deliberately narrow ABI boundary inspired by Psi's host/accessor model.

## Core rule

**No MatterLeast core type is part of the plugin ABI.**

Headers under `plugins/api/` may depend on Qt ABI types, but must not include anything from `sources/`. In particular, plugins must never receive `Backend`, `BackendChannel`, `Storage`, `ChatArea`, `PostWidget`, `NavigationUiController`, or other internal objects.

The boundary has two directions:

- **host/accessor pairs** expose a small service from MatterLeast to plugins;
- **extension interfaces** expose a plugin capability that MatterLeast may call.

The core implements adapters between these stable interfaces and the current internal architecture.

## Standalone SDK

The plugin API is an installable CMake package, not a requirement to build inside the MatterLeast source tree. Its API version is independent from the MatterLeast application version.

After installing the `Development` component, an external plugin project uses:

```cmake
find_package(MatterLeastPluginApi 1.0 CONFIG REQUIRED)

target_link_libraries(my-plugin
    PRIVATE
        MatterLeast::PluginApi
)
```

Installed public headers live under `matterleast/plugin/`, for example:

```cpp
#include <matterleast/plugin/PluginInterface.h>
#include <matterleast/plugin/ConversationAccessor.h>
#include <matterleast/plugin/ConversationMenuInterface.h>
```

The installed package exports only the stable API target and its Qt Core dependency. It exports no MatterLeast core target or include directory.

CI contains a separate external smoke project that first installs the SDK to a clean prefix and then performs a new CMake configure/build using only `find_package(MatterLeastPluginApi ...)`. This guards against accidentally making plugin builds depend on the MatterLeast source tree.

## Lifecycle

Every native plugin implements `PluginInterface/1.0` and is a Qt plugin loaded by `QPluginLoader`.

1. Metadata is inspected without instantiating the plugin.
2. The library is loaded and cast to `PluginInterface`.
3. Every accessor interface implemented by the plugin receives its corresponding host interface.
4. Only after host injection does MatterLeast call `enable()`.
5. On shutdown/unload, `disable()` is called before the library is unloaded.

`PluginHost` explicitly allows unloading instead of relying on Qt's default `PreventUnloadHint`, and `PluginManager` shuts plugins down while `Backend` is still alive. A plugin may therefore use injected hosts during `disable()` without observing partially destroyed core state.

A plugin may implement only the capabilities it needs. It does not receive a general `PluginContext` or a pointer to the application/core.

## ABI versioning

Each capability has its own IID and version, for example:

- `io.github.Ri0n.MatterLeast.Plugin/1.0`
- `io.github.Ri0n.MatterLeast.ConversationAccessor/1.0`
- `io.github.Ri0n.MatterLeast.ConversationAccessingHost/1.0`
- `io.github.Ri0n.MatterLeast.ConversationMenu/1.0`

Do not mutate an incompatible published interface in place. Introduce a new IID/version and let the core support old and new versions in parallel while compatibility is required.

The CMake package currently reports plugin API version `1.0.0`. This is deliberately not the same version namespace as the MatterLeast application release.

The ABI guarantee is intentionally scoped to these interfaces and the compatible Qt/toolchain ABI used to build the plugin. It is not an ABI guarantee for MatterLeast internals.

## Metadata

Plugins use `Q_PLUGIN_METADATA` and currently require these fields in the metadata object:

```json
{
  "id": "org.example.plugin",
  "name": "Example plugin",
  "version": "1.0.0"
}
```

`id` must be globally stable. Duplicate IDs are rejected.

## Discovery

The first implementation discovers trusted native plugins in:

1. `<application directory>/plugins`
2. the platform `QStandardPaths::AppDataLocation/plugins` directory

Native plugins execute in-process and are **not sandboxed**. A permissions UI, enable/disable persistence and plugin distribution are separate layers and must not weaken the ABI boundary described here.

## Conversation capability

The first dogfood capability is the conversation/thread header menu.

`ConversationAccessor` gives a plugin a `ConversationAccessingHost`, currently providing stable operations for:

- conversation display name;
- compact Mattermost reference (`@user` / `~channel` where available);
- opening a conversation/thread;
- opening it in a new tab.

`ConversationMenuInterface` lets a plugin contribute menu actions. It returns `QVariantHash` descriptors instead of application `QAction` objects or a custom ABI struct. This keeps ownership inside MatterLeast and lets optional action properties evolve by adding keys without changing the C++ interface layout.

Supported action descriptor keys are documented in `plugins/api/ConversationMenuInterface.h`.

MatterLeast creates proxy `QAction`s and resolves the plugin object again when an action is triggered. UI objects therefore never cross the plugin boundary and stale actions cannot retain a raw plugin interface pointer after unload.

## Ownership

- `PluginManager` owns loaded `PluginHost` instances.
- `PluginHost` owns the `QPluginLoader` lifecycle for one plugin.
- host adapters translate plugin API calls into current core services.
- `ChatArea` owns only the generated proxy actions placed in its menu.

Do not move core implementation logic into `plugins/api/`. That directory is the stable SDK boundary, not a shared internal helper library.
