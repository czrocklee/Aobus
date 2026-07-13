---
id: persistence.location
type: reference
status: current
domain: persistence
summary: Enumerates default Linux locations for Aobus managed state, durable data, user configuration, logs, and caches.
---
# Managed file locations

## Scope and version

This reference enumerates the current default Linux paths selected by the GTK, TUI, and CLI composition roots.
It owns locations and path overrides, not the schemas or behavior of the files stored there.

The path surface is not independently versioned.
Serialized compatibility belongs to each payload's format owner, while ownership and lifecycle belong to the [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) places path selection in frontend composition roots.
GTK resolves GLib/XDG locations and library-relative paths, TUI resolves command-line and library-relative paths, and CLI resolves only its selected music root and library database.
Runtime and UIModel receive paths or stores and do not discover platform application directories.

## Surface

### Path notation

| Token | Linux meaning |
|---|---|
| `<config>` | `Glib::get_user_config_dir()`, normally `$XDG_CONFIG_HOME` or `~/.config` |
| `<state>` | `$XDG_STATE_HOME` when non-empty; otherwise the `state` sibling of `Glib::get_user_data_dir()`, normally `~/.local/state` |
| `<cache>` | `Glib::get_user_cache_dir()`, normally `$XDG_CACHE_HOME` or `~/.cache` |
| `<root>` | The selected music-library root |
| `<preset-id>` | A validated shell-layout preset identifier |

### Global GTK application locations

| Location | Class | Writer or reader |
|---|---|---|
| `<config>/aobus/config.yaml` | Global managed state for GTK application preferences, window/session state, keymap overrides, and the active library's playback session | `AppConfigStore` and the runtime playback-session owner through its borrowed store |
| `<config>/aobus/layouts/<preset-id>.yaml` | One user-customized shell layout document | `ShellLayoutStore` |
| `<config>/aobus/user.css` | Optional user-authored GTK style override | `GtkStyleRuntime` reads and monitors it; Aobus does not generate it |
| `<state>/aobus/layout-state/<preset-id>.yaml` | Per-preset shell component runtime state | `ShellLayoutComponentStateStore` |
| `<cache>/aobus/logs/` | GTK operational logs | Runtime logging configured by the GTK composition root |
| `<cache>/aobus/mpris-art/` | Exported cover-art files used by MPRIS file URLs | `MprisArtUrlCache` |

### Per-library locations

| Location | Frontend | Class | Override |
|---|---|---|---|
| `<root>/.aobus/library/` | GTK, TUI, and CLI | Default LMDB music-library database | TUI `--database` may select another database path; CLI derives this path from `-C`/`--root` or `AOBUS_ROOT` |
| `<root>/.aobus/library/workspace.yaml` | GTK | Runtime workspace and view session | None in the current GTK command surface |
| `<root>/.aobus/gtk_layout.yaml` | GTK | Per-library track-column and list-presentation preferences | None |
| `<root>/.aobus/tui-workspace.yaml` | TUI | Default workspace and playback-session `ConfigStore` | `--config` selects another file |
| `<root>/.aobus/logs/` | TUI | TUI operational logs | The library root changes the base location |

GTK's workspace file is inside its default database directory because the GTK composition root derives it as `<database-path>/workspace.yaml`.
The current GTK database path is `<root>/.aobus/library/`.

TUI passes one store as the owned workspace store and does not inject a separate playback-session store.
`AppRuntime` therefore uses the selected TUI configuration file for both managed-state groups.

### CLI and interchange files

CLI opens `<root>/.aobus/library/` and does not load the interactive workspace, playback-session, GTK application, or GTK presentation files.
Its root is selected through `-C`/`--root` or `AOBUS_ROOT`.

Library YAML imports and exports use user-selected input or output paths.
They are interchange artifacts rather than managed application locations; their shape belongs to the [library YAML format](../library/format/yaml.md), and their behavior belongs to the [YAML transfer specification](../../spec/library/runtime/yaml-transfer.md).

Development build trees, managed tool environments, and SDK caches are repository tooling state rather than product data.
Linux defaults and `AOBUS_BUILD_ROOT` are described in the repository [README](../../../README.md), while native Windows locations and overrides belong to [Windows development](../../development/windows.md#local-state-and-build-trees).

## Validation rules

- `ShellLayoutStore` rejects an empty preset id and ids containing `/`, `\`, or `..` before constructing a path.
- `ShellLayoutComponentStateStore` additionally rejects preset ids containing a null byte.
- A TUI `--database` override changes the database path without changing the selected music root.
- A TUI `--config` override changes the workspace/playback-session file without changing its payload ownership.
- TUI normalizes its selected root and override paths to absolute lexical paths before runtime composition.
- CLI passes its selected root to runtime and derives the database path from that value without a separate interactive configuration store.
- A library YAML export path never becomes an application-managed path merely because its encoding is YAML.

Observable missing-file, parse, fallback, and save behavior belongs to the relevant specifications and semantic owners rather than this location inventory.

## Compatibility and versioning

A path override changes only location.
It does not change the schema, semantic owner, store-sharing rule, or restore/save lifecycle of the payload.

Global GTK playback state is paired with the last selected library by the [interactive session lifecycle architecture](../../architecture/interactive-session-lifecycle.md) even though it contains library-local identities.
Workspace and presentation state remain physically per-library so those identities do not migrate to another root through a global preference file.

## Implementation authority

- [`app/linux-gtk/main.cpp`](../../../app/linux-gtk/main.cpp) resolves global GTK config, layout, component-state, log, database, and workspace locations.
- [`GtkStyleRuntime.cpp`](../../../app/linux-gtk/app/GtkStyleRuntime.cpp) resolves `user.css`.
- [`MprisArtUrlCache.cpp`](../../../app/linux-gtk/platform/MprisArtUrlCache.cpp) resolves the MPRIS artwork cache.
- [`MainWindowCoordinator.cpp`](../../../app/linux-gtk/app/MainWindowCoordinator.cpp) supplies the per-library `.aobus` directory to `GtkLayoutStateStore`.
- [`app/tui/Main.cpp`](../../../app/tui/Main.cpp) owns TUI root, database, and configuration defaults and overrides.
- [`app/tui/App.cpp`](../../../app/tui/App.cpp) resolves the TUI log location and constructs its runtime store.
- [`CliRuntime.cpp`](../../../app/cli/CliRuntime.cpp) derives the CLI library database from its selected root.

## Test authority

- [`AppConfigStoreTest.cpp`](../../../test/unit/linux-gtk/app/AppConfigStoreTest.cpp) protects the global GTK file boundary.
- [`ShellLayoutStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutStoreTest.cpp) and [`ShellLayoutComponentStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/ShellLayoutComponentStateStoreTest.cpp) protect preset file construction and traversal rejection.
- [`GtkLayoutStateStoreTest.cpp`](../../../test/unit/linux-gtk/app/GtkLayoutStateStoreTest.cpp) protects the per-library GTK presentation file.
- [`AtomicFileTest.cpp`](../../../test/unit/utility/AtomicFileTest.cpp) protects replacement and owner-only permission behavior used by managed YAML files.
- [`CliSmokeTest.cpp`](../../../test/unit/cli/CliSmokeTest.cpp) protects CLI root use around the runtime boundary.
- TUI option defaults are exercised through the TUI application and tooling build/test gates; no focused path-surface test currently locks every TUI override.

## Related documents

- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Library architecture](../../architecture/library.md)
- [Playback architecture](../../architecture/playback.md)
- [Presentation architecture](../../architecture/presentation.md)
- [Application managed-state surface](application-config.md)
- [Atomic file replacement specification](../../spec/persistence/atomic-replacement.md)
- [Library database reference](../library/storage/database.md)
- [List presentation preference specification](../../spec/presentation/list-preference.md)
