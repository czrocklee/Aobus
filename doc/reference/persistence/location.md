---
id: persistence.location
---
# Managed file locations

## Scope and version

This reference enumerates the current default paths selected by the GTK, TUI,
WinUI, AppKit, and CLI composition roots. Helper-based Linux/Windows paths and
native AppKit locations are distinguished below; the [Windows desktop state
reference](../windows/desktop-state.md) owns Windows settings and theme formats.
It owns locations and path overrides, not the schemas or behavior of the files stored there.

The path surface is not independently versioned.
Serialized compatibility belongs to each payload's format owner, while ownership and lifecycle belong to the [persistence and managed-state architecture](../../system/persistence/README.md).

## Code boundary

The [system architecture](../../system/overview.md) places platform selection and runtime construction in frontend composition roots.
Runtime `LibraryPaths` derives the canonical per-library managed-data base, database path, log path, and existing-database probe from a supplied music root without discovering platform application directories.
GTK resolves GLib/XDG locations, its selected music root, and frontend-specific files; TUI resolves its selected root and command-line overrides; WinUI resolves its Windows locations; and CLI resolves its selected music root.
Every composition root, the CLI included, also resolves the application cache directory and passes it to the runtime, because the runtime discovers no platform directory itself.
UIModel receives paths or stores and does not resolve storage locations.

## Surface

### Path notation

| Token | Linux meaning | Windows meaning |
|---|---|---|
| `<config>` | `utility::applicationConfigDirectory()`, which reads `$XDG_CONFIG_HOME/aobus`, then `$HOME/.config/aobus`, then the account entry's `.config/aobus`, and accepts only absolute values | `utility::applicationConfigDirectory()`, which reads `%LOCALAPPDATA%\Aobus` then `%APPDATA%\Aobus`, accepting only absolute values |
| `<state>` | `$XDG_STATE_HOME` when non-empty; otherwise the `state` sibling of `Glib::get_user_data_dir()`, normally `~/.local/state` | Not used |
| `<cache>` | `Glib::get_user_cache_dir()`, normally `$XDG_CACHE_HOME` or `~/.cache` | Not used |
| `<app-cache>` | `utility::applicationCacheDirectory()`, which reads `$XDG_CACHE_HOME/aobus`, then `$HOME/.cache/aobus`, then the account entry's `.cache/aobus` | `utility::applicationCacheDirectory()`, which reads `%LOCALAPPDATA%\Aobus\Cache` |
| `<root>` | The selected music-library root | The selected music-library root |
| `<preset-id>` | A validated shell-layout preset identifier | A validated shell-layout preset identifier |

`<app-cache>` is deliberately local-machine on Windows: a roaming profile would synchronize derived files between machines that can each rebuild them.
A root that cannot resolve it composes a runtime with no cache directory rather than failing to start.

GTK, TUI, and WinUI resolve `<config>` through the same helper. The POSIX
`<config>` and `<app-cache>` helper rules also apply to macOS TUI/CLI consumers;
native AppKit instead selects its own Foundation application-state root below.
When nothing names a home or profile location at all, GTK and the TUI do not substitute an empty path or the process working directory: they compose no-location stores for survivable application state and run initially on defaults.
TUI also chooses that degraded application store when `<config>` resolves but its directory cannot be created.
The command-line-selected TUI workspace/playback location is separate and must still be prepared successfully.
The Windows shell reports an application-directory resolution or preparation failure instead, because required state lives there in addition to preferences.
The [grouped configuration store specification](../../system/persistence/config-store.md#sessions-with-no-location) owns the no-filesystem and same-instance store consequences; the [application config reference](application-config.md#frontend-policy-without-a-persistent-application-location) owns each frontend's save and startup policy.

### Global GTK application locations

| Location | Class | Writer or reader |
|---|---|---|
| `<config>/config.yaml` | Global managed state for GTK application preferences, window/session state, keymap overrides, and the active library's playback session | `AppConfigStore` and the runtime playback-session owner through its borrowed store |
| `<config>/layouts/<preset-id>.yaml` | One user-customized shell layout document | `ShellLayoutStore` |
| `<config>/user.css` | Optional user-authored GTK style override | `GtkStyleRuntime` reads and monitors it; Aobus does not generate it |
| `<state>/aobus/layout-state/<preset-id>.yaml` | Per-preset shell component runtime state | `ShellLayoutComponentStateStore` |
| `<cache>/aobus/logs/` | GTK operational logs | Runtime logging configured by the GTK composition root |

### Cross-frontend derived cache

| Location | Class | Writer or reader |
|---|---|---|
| `<app-cache>/cover/` | Derived cover-art cache for GTK, TUI, WinUI, and CLI libraries using this cache root | `ResourceByteDiskCache`, constructed by the runtime from the directory its composition root supplied |
| `<cache>/aobus/mpris-art-v2/` | Linux GTK/TUI MPRIS file URLs, named by full SHA-256 plus detected extension and published owner-only | `MprisArtUrlCache` in each system-media-enabled process |

The MPRIS directory is shared by Linux frontends but contains only discardable delivery artifacts. Different `ResourceId` handles for equal bytes converge on the same digest path; no ResourceId-named sibling cleanup occurs. A process-memoized entry receives regular-file and byte-size validation, not a readback digest check.
The unused legacy `<cache>/aobus/mpris-art/` directory is retained, and the current MPRIS directory has no automatic size limit or eviction; see [MPRIS artwork retention and manual cleanup](../../system/frontend/mpris.md#artwork-delivery).

Derived cover-cache entries are named by content digest, so two libraries using the same cache root
share one entry for the same cover and neither can serve the other a wrong image.
AppKit supplies a separate cache root, listed below.
The directory is discardable: deleting it changes no library fact and can change only what is displayed when every audio file carrying a cover is already gone.
The [cover-art delivery specification](../../system/resource/cover-art-delivery.md) owns its budget, eviction, and verification behavior.

### Global TUI application locations

The TUI keeps its own file rather than sharing GTK's. `ConfigStore` writes a whole document from the snapshot it took at first read, so two frontends pointed at one file would drop each other's groups whenever they ran at the same time.

| Location | Class | Writer or reader |
|---|---|---|
| `<config>/tui.yaml` | Global managed state for TUI application preferences and editable shortcut overrides | One `ConfigStore` owned by the TUI composition root |

The TUI loads the `shortcuts` group from this global file, independently of the selected library and `--config` workspace override.
Settings writes accepted shortcut and UI-preference candidates through that same store, preserving sibling groups. Ordinary exit does not rewrite untouched shortcuts.

### Native AppKit application locations

`<appkit-state>` is the user Application Support directory plus `Aobus/macos`,
normally `~/Library/Application Support/Aobus/macos`. `--state-root <path>`
selects another absolute location; failure to resolve the default is a startup
error. It is not the shared POSIX `<config>` directory.

| Location | Class | Writer or reader |
|---|---|---|
| `<appkit-state>/desktop.plist` | Native desktop settings, including selected root and window state | `DesktopApplication`; Foundation property-list persistence, not `ConfigStore` YAML |
| `<appkit-state>/desktop.lock` | Exclusive application-state lease | `ApplicationStateLease` |
| `<appkit-state>/empty-library/` | Empty startup library when no root is selected | Desktop startup planning |
| `<appkit-state>/logs/` | AppKit operational logs | Runtime logging configured by `DesktopApplication` |
| `<appkit-state>/cache/cover/` | Derived cover cache shared by AppKit libraries using this state root | Runtime `ResourceByteDiskCache` |

AppKit workspace and playback state are per-library, not in `desktop.plist`.
The [session lifecycle](../../system/session-lifecycle.md#appkit) owns startup,
checkpoint, and successor persistence admission.

### Per-library locations

| Location | Frontend | Class | Override |
|---|---|---|---|
| `<root>/.aobus/library/` | GTK, TUI, WinUI, AppKit, and CLI | Default LMDB music-library database | TUI `--database` may select another database path; CLI derives this path from `-C`/`--root` or `AOBUS_ROOT` |
| `<root>/.aobus/library/workspace.yaml` | GTK and WinUI | Runtime workspace and view session | None in the current GTK or WinUI command surface |
| `<root>/.aobus/library/appkit-workspace.yaml` | AppKit | Runtime workspace and playback-session groups in one owned `ConfigStore` | Selected library root |
| `<root>/.aobus/gtk_layout.yaml` | GTK | Per-library desktop track-column and list-presentation preferences | None |
| `<root>/.aobus/tui_layout.yaml` | TUI | Per-library List navigation visibility, sidebar widths, terminal-cell track-column and list-presentation preferences | None |
| `<root>/.aobus/winui_layout.yaml` | WinUI | Per-library desktop track-column and list-presentation preferences | None |
| `<root>/.aobus/tui-workspace.yaml` | TUI | Default workspace and playback-session `ConfigStore` | `--config` selects another file |
| `<root>/.aobus/logs/` | TUI | TUI operational logs | The library root changes the base location |

GTK's workspace file is inside its default database directory because the GTK composition root derives it as `<database-path>/workspace.yaml`.
The current GTK database path is `<root>/.aobus/library/`.

TUI passes one store as the owned workspace store and does not inject a separate playback-session store.
`AppRuntime` therefore uses the selected TUI configuration file for both managed-state groups.
The TUI layout file is always derived from the selected root and is independent of `--config`; one `LayoutStateStore` writer owns its `navigation`, `panels`, and both presentation groups.

WinUI derives its database, workspace, and layout files from the root it opens. The layout store only exists once a library is open; the global `windows-settings.yaml` described by the [Windows desktop state reference](../windows/desktop-state.md) holds no list-keyed state.

### CLI and interchange files

CLI opens `<root>/.aobus/library/` and does not load the interactive workspace, playback-session, application-preference, GTK presentation, or TUI presentation files.
Its root is selected through `-C`/`--root`, then `AOBUS_ROOT`, then the current directory.

Library YAML imports and exports use user-selected input or output paths.
They are interchange artifacts rather than managed application locations; their shape belongs to the [library YAML format](../library/format/yaml.md), and their behavior belongs to the [YAML transfer specification](../../system/library/yaml-transfer.md).

Development build trees, managed tool environments, and SDK caches are repository tooling state rather than product data.
Linux defaults and `AOBUS_BUILD_ROOT` are described in [build development](../../development/build.md), while native Windows locations and overrides belong to [Windows development](../../development/windows.md#local-state-and-build-trees).

## Validation rules

- `ShellLayoutStore` rejects an empty preset id and ids containing `/`, `\`, or `..` before constructing a path.
- `ShellLayoutComponentStateStore` additionally rejects preset ids containing a null byte.
- A TUI `--database` override changes the database path without changing the selected music root.
- A TUI `--config` override changes the workspace/playback-session file without changing its payload ownership, the independent `tui_layout.yaml` location, or the global shortcut source; startup rejects an override that aliases the TUI layout or global application-preference file.
- TUI normalizes its selected root and override paths to absolute lexical paths before runtime composition.
- CLI passes its selected root to `LibraryPaths` and opens the derived database without a separate interactive configuration store.
- `LibraryPaths::hasExistingDatabase()` detects a database created at the canonical location without exposing the LMDB marker filename to a frontend.
- `applicationCacheDirectory()` has no caller under `app/runtime/`; a runtime constructed with no cache directory consults none and still delivers covers.
- A library YAML export path never becomes an application-managed path merely because its encoding is YAML.

Observable missing-file, parse, fallback, and save behavior belongs to the relevant specifications and semantic owners rather than this location inventory.

## Compatibility and versioning

A path override changes only location.
It does not change the schema, semantic owner, store-sharing rule, or restore/save lifecycle of the payload.

Global GTK playback state is paired with the last selected library by the [interactive session lifecycle architecture](../../system/session-lifecycle.md) even though it contains library-local identities.
Workspace and presentation state remain physically per-library so those identities do not migrate to another root through a global preference file.

## Implementation authority

- [`LibraryPaths.h`](../../../app/include/ao/rt/library/LibraryPaths.h) and [`LibraryPaths.cpp`](../../../app/runtime/library/LibraryPaths.cpp) own the canonical per-library managed-data base, database, log, and existing-database probe.
- [`app/linux-gtk/main.cpp`](../../../app/linux-gtk/main.cpp) resolves global GTK config, layout, component-state, log, selected-root, and workspace locations.
- [`GtkStyleRuntime.cpp`](../../../app/linux-gtk/app/GtkStyleRuntime.cpp) resolves `user.css`.
- [`MprisArtUrlCache.cpp`](../../../app/platform/media/linux/MprisArtUrlCache.cpp) resolves the shared Linux MPRIS artwork cache.
- [`ResourceByteDiskCache.cpp`](../../../app/runtime/resource/ResourceByteDiskCache.cpp) owns the shared derived cover-cache layout below the supplied runtime cache directory.
- [`MainWindow.cpp`](../../../app/linux-gtk/app/MainWindow.cpp) appends the GTK presentation filename to the canonical per-library managed-data path.
- [`app/tui/Main.cpp`](../../../app/tui/Main.cpp) owns TUI root, database, and configuration override selection and appends its frontend-specific configuration filename.
- [`app/tui/App.cpp`](../../../app/tui/App.cpp) uses the canonical per-library log path, requires preparation of the selected workspace directory, and selects either `<config>/tui.yaml` or a no-location application store after resolver or directory-creation failure; [`LayoutStateStore.cpp`](../../../app/tui/LayoutStateStore.cpp) appends the TUI presentation filename to the canonical managed-data path.
- [`LibrarySession.cpp`](../../../app/windows-winui/app/LibrarySession.cpp) opens the canonical database, places the workspace file in that database directory, resolves the Windows state root, and appends the WinUI presentation filename to the canonical per-library managed-data path.
- AppKit [`DesktopMain.mm`](../../../app/macos-appkit/DesktopMain.mm), [`DesktopApplication.mm`](../../../app/macos-appkit/DesktopApplication.mm), and [`LibrarySession.cpp`](../../../app/macos-appkit/LibrarySession.cpp) own native state-root selection, property-list settings, cache injection, and the per-library workspace/playback file.
- [`CliRuntime.cpp`](../../../app/cli/CliRuntime.cpp) opens the canonical database for its selected root and resolves the cache directory it passes to the runtime.
- [`PlatformDirectories.h`](../../../include/ao/utility/PlatformDirectories.h), [`PlatformDirectoriesPosix.cpp`](../../../lib/utility/PlatformDirectoriesPosix.cpp), and [`PlatformDirectoriesWindows.cpp`](../../../lib/utility/PlatformDirectoriesWindows.cpp) own the config and cache resolvers.
- [`LibraryWindowLifecycle.cpp`](../../../app/linux-gtk/app/LibraryWindowLifecycle.cpp), [`app/tui/App.cpp`](../../../app/tui/App.cpp), and [`LibrarySession.cpp`](../../../app/windows-winui/app/LibrarySession.cpp) resolve the same cache directory for their frontends.
- [`ArchitectureAudit.cmake`](../../../app/cmake/ArchitectureAudit.cmake) rejects canonical `.aobus` and LMDB marker literals in frontend C++ source.

## Test authority

- [`LibraryPathsTest.cpp`](../../../test/unit/runtime/library/LibraryPathsTest.cpp) locks the exact canonical paths and detects a database created through `MusicLibrary` without duplicating its physical marker.
- [`AppConfigStoreTest.cpp`](../../../test/unit/linux-gtk/app/AppConfigStoreTest.cpp) protects the global GTK file boundary.
- [`ShellLayoutStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutStoreTest.cpp) and [`ShellLayoutComponentStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutComponentStateStoreTest.cpp) protect preset file construction and traversal rejection.
- [`GtkLayoutStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/GtkLayoutStateStoreTest.cpp) protects the per-library GTK presentation file.
- [`AtomicFileTest.cpp`](../../../test/unit/utility/AtomicFileTest.cpp) protects replacement and owner-only permission behavior used by managed YAML files.
- [`CliSmokeTest.cpp`](../../../test/unit/cli/CliSmokeTest.cpp) protects CLI root use around the runtime boundary.
- [`LayoutStateStoreTest.cpp`](../../../test/unit/tui/LayoutStateStoreTest.cpp) protects the exact per-library TUI presentation path; TUI option defaults are exercised through the TUI application and tooling build/test gates, and no focused test currently locks every override.

## Related documents

- [Persistence and managed-state architecture](../../system/persistence/README.md)
- [Library architecture](../../system/library/structure.md)
- [Playback architecture](../../system/playback/README.md)
- [Presentation architecture](../../system/presentation/README.md)
- [Application managed-state surface](application-config.md)
- [Atomic file replacement specification](../../system/persistence/atomic-replacement.md)
- [Library database reference](../library/storage/database.md)
- [List presentation preference specification](../../system/presentation/list-preference.md)
