---
id: architecture.persistence-and-managed-state
type: architecture
status: current
domain: persistence
summary: Defines durable-data and managed-state ownership, encoding, storage, composition, and lifecycle boundaries.
---
# Persistence and managed-state architecture

## Scope

This document owns how Aobus classifies persisted state, assigns one authority to each persistence domain, and moves typed managed state through codecs, stores, paths, and lifecycle policy.
It distinguishes durable library truth, application configuration and session state, authored presentation documents, transient component state, user-selected interchange artifacts, and regenerable operational data.

It does not define exact file paths, YAML groups and fields, LMDB database names or records, version numbers, default values, restore matrices, or retry schedules.
Those exact surfaces and behavioral contracts belong in reference and specifications.

Command-line syntax, user-authored CSS semantics, and library YAML import/export behavior also remain outside this architecture even when they select or produce a file.

## System context

Persistence follows state ownership rather than a universal store:

```text
music root
  -> MusicLibrary database                  durable library truth
  -> runtime workspace ConfigStore          per-library application session
  -> GTK library presentation store         per-library UI preference
  -> TUI config store                       configurable TUI workspace/session

user application directories
  -> AppConfigStore                         global GTK preferences/session
  -> ShellLayoutStore                       customized layout documents
  -> ShellLayoutComponentStateStore         transient component state
  -> caches/logs                             regenerable operational data
```

Typed managed state follows a common architectural pipeline:

```text
runtime / UIModel / frontend semantic owner
  -> typed candidate or immutable snapshot
  -> model codec or application configuration traits
  -> ConfigStore or a specialized file store
  -> ao::yaml parsing/emission + AtomicFile replacement
  -> path selected by the composition root
```

The second diagram shows the save direction.
Load traverses the same boundary in reverse until the semantic owner validates a candidate and decides whether it can replace live state.

The principal code boundaries refine the layer model in the [system architecture](system-overview.md):

| Concern | System layer | Public boundary | Implementation |
|---|---|---|---|
| Durable library data | Core libraries | `ao::library::MusicLibrary` | `include/ao/library/` and `lib/library/` over LMDB |
| YAML and atomic-file mechanisms | Core libraries | `include/ao/yaml/` and `include/ao/utility/AtomicFile.h` | Header adapters and `lib/utility/AtomicFile.cpp` |
| Grouped managed file | Application runtime | `app/include/ao/rt/ConfigStore.h` | `app/runtime/ConfigStore.cpp` and `app/include/ao/yaml/ConfigTraits.h` |
| Runtime session semantics | Application runtime | `WorkspaceService`, `AppRuntime`, and playback-session commands | Runtime workspace and playback-persistence implementations under the [workspace](workspace.md), [interactive session lifecycle](interactive-session-lifecycle.md), and playback owners |
| Platform-neutral presentation state | UIModel | Input, layout, and library-presentation store/model headers | `app/uimodel/` model codecs and state helpers |
| Paths and platform stores | GTK and TUI frontends | Frontend-local composition and store adapters | `app/linux-gtk/app/`, `app/linux-gtk/main.cpp`, and `app/tui/` |

`ao::yaml` is a reusable mechanism, not a persistence service or schema authority.
`ConfigStore` is likewise a grouped file mechanism rather than a facade over every persisted domain.

## Responsibilities

### Persistence classes

`ao::library::MusicLibrary` exclusively owns the database for tracks, lists, resources, dictionary values, file identity, and library metadata.
Runtime facades coordinate access but do not mirror that truth into frontend configuration.

Library YAML imports and exports are explicit user-selected interchange artifacts.
They are not managed application state and retain separate format and transfer owners.

Managed state records typed application preferences, workspace/session intent, or UI state that Aobus restores and saves through an assigned owner.
Authored layout documents remain separate from layout component runtime state so user intent can be reset, pruned, or promoted deliberately.

Logs, exported MPRIS artwork, and similar cache artifacts are operational output rather than authoritative product state.
Their owners may recreate or discard them without changing library, workspace, playback-session, or presentation semantics.

### Managed-state owners

The component whose live behavior depends on a managed value owns its typed model, defaults, semantic validation, compatibility decisions, restore commit, and save trigger.
Persistence through a generic codec does not transfer that responsibility to the codec or file store.

| State family | Semantic and lifecycle owner | File and path owner |
|---|---|---|
| Runtime workspace and custom track presentations | `WorkspaceService` | The `ConfigStore` owned by `AppRuntime`, at the path injected by GTK or TUI |
| Restorable playback intent | `PlaybackSessionPersistence` behind `AppRuntime` | The injected playback-session `ConfigStore`; it may be the workspace store or a separately owned store |
| Global GTK window, preferences, application session, and keymap state | The GTK workflow plus the runtime/UIModel value owner for each payload | `AppConfigStore` and the GTK composition root |
| Customized shell layout documents | UIModel layout document and validation code, coordinated by the GTK shell-layout workflow | `ShellLayoutStore`, one document per preset |
| Shell layout component runtime state | UIModel component-state model, pruning, and promotion rules | `ShellLayoutComponentStateStore` |
| Per-library column and list-presentation preferences | UIModel presentation state and the GTK window workflow | `GtkLayoutStateStore` |

Global state may contain per-library track or list identities only when lifecycle ownership pairs that payload with the active library and validates those identities before restore.
Unrelated global preferences cannot retain such identities.

### Codecs and schema policy

`ConfigStore` templates use the application configuration traits in `app/include/ao/yaml/ConfigTraits.h` to encode common scalar, container, strong-id, enum, and aggregate shapes.
This reflection-based codec is a convenience for the current representation; C++ member names and enum ordinals do not become durable compatibility policy merely because the codec can write them.

A model that needs explicit field names, stable identifiers, version detection, strict decoding, migration, or object-level fallback owns a focused codec beside the model.
The UIModel layout and component-state areas add model-specific YAML adapters, while playback-session restore selects strict aggregate decoding and performs semantic validation above it.

The current authored shell-layout codec reads its numeric version without rejecting unsupported values and has no shell-level file/model/expansion budget.
[RFC 0025](../rfc/0025-bounded-shell-layout-documents.md) proposes a strict candidate boundary owned by the shell format rather than by generic YAML or `ConfigStore`.

The schema owner decides whether absence preserves seeded defaults, whether unknown data is tolerated, whether malformed data rejects the whole object, and whether an older representation can be migrated.
`ConfigStore` only applies the selected decode operation to a group.

### Store and composition owners

`ConfigStore` is the application-runtime mechanism for multiple named groups that share one whole-file document and writer authority.
The [grouped configuration store specification](../spec/persistence/config-store.md) owns its lazy initialization, decode modes, group operations, flush behavior, failures, and concurrency contract.
The semantic owner above the store remains responsible for dirty state, scheduling, retry, observation, cross-field validation, fallback, and durability acknowledgement.

A specialized store may bypass `ConfigStore` when its document boundary, synchronization, or pruning behavior differs from grouped application configuration.
`ShellLayoutComponentStateStore`, for example, reads and emits one complete component-state document directly, protects its operations with a mutex, and still uses the shared YAML and atomic-file mechanisms.

GTK and TUI resolve platform or command-line paths, construct stores before `AppRuntime`, and keep any borrowed store alive until runtime persistence has shut down.
`AppRuntime` owns its workspace store and borrows an explicitly supplied playback-session store; when none is supplied, playback session and workspace use the same owned instance.

GTK supplies its global `AppConfigStore` as the playback-session store while supplying a per-library workspace store separately.
TUI currently uses its selected `ConfigStore` for both roles.
CLI opens the library database for the selected root but does not load interactive managed state.

## Boundaries and dependency direction

- Core storage, YAML, and atomic-file mechanisms cannot depend on runtime, UIModel, a frontend, or any application schema.
- Runtime persistence may depend on Core mechanisms but cannot depend on UIModel or platform paths.
- UIModel may define typed serializable values and codecs and may use the runtime `ConfigStore` along the top-level dependency direction, but it cannot resolve GTK, terminal, or XDG locations.
- Frontends may own paths and file adapters, but cross-frontend runtime state remains owned and validated by runtime.
- A generic codec or store cannot become the semantic owner of a group merely because it names, serializes, or flushes that group.
- Library LMDB truth and library YAML interchange cannot be routed through `ConfigStore`; they retain their own storage and format owners.
- Each durable location has one application-level writer authority.
- `ConfigStore` has no internal synchronization contract; all access to one instance is confined to its owning executor or externally serialized.
- Multiple groups in one file share one mutable document and therefore one writer authority.
  Independent writers targeting the same path would each hold a stale whole-file tree and can overwrite one another's groups.
- A specialized store may define a different synchronization contract, but that contract remains local to the specialized type.

## Data and control flow

### Composition and restore

```text
frontend resolves platform/root paths
  -> constructs owned and borrowed stores
  -> constructs CoreRuntime or AppRuntime
  -> store or specialized file adapter establishes its managed document
  -> owner selects its document or top-level group
  -> codec decodes into a seeded value or separate candidate
  -> semantic owner validates identities, versions, and cross-field invariants
  -> owner commits, normalizes, falls back, or retains current/default state
```

Missing data is not automatically an error or automatically a default.
The semantic owner decides whether absence means first run, no customization, no restorable session, or a required document failure.

The store specification defines ordinary and exact decode mechanics.
Features that require all-or-nothing restore still prepare a candidate and install it only after the owning codec and semantic validation have succeeded.

### Save

```text
semantic owner captures one coherent typed value
  -> model codec encodes the owned group or document
  -> store mutates its in-memory tree
  -> explicit flush emits the complete file
  -> AtomicFile replaces the previous file
  -> semantic owner acknowledges only the successfully durable snapshot
```

Atomic replacement protects the target path from a helper-written partial replacement under the [platform replacement contract](../spec/persistence/atomic-replacement.md).
It does not make separate group mutations a semantic transaction, serialize concurrent writers, or acknowledge a newer in-memory revision on behalf of the state owner.

Playback-session persistence adds dirty revisions, debounce, retry, and final checkpoint policy above this mechanism.
Workspace, GTK preference, layout, and presentation owners currently use their own explicit lifecycle save points.

### Library switching

Library switching in GTK tears down the current library runtime and its per-library stores, records the newly selected library in global application state, and constructs a replacement runtime for that library.
Global layout customization and application preferences survive the replacement; library database, workspace, and per-library presentation state change with the selected root.
The [interactive session lifecycle architecture](interactive-session-lifecycle.md) owns the orchestration and lifetime order; this document owns which state and stores survive that transition, while the [workspace architecture](workspace.md) owns the workspace candidate's semantics.

## Structural constraints

- Library truth is never reconstructed from UI configuration when the library database is available.
- Every managed group or standalone document has one semantic schema owner and one file-writer owner; these roles may be different components.
- Global application state and per-library state have separate owners and lifetimes.
- One process path has one active writer authority, and sibling groups in a shared file are mutated through the same live document.
- Defaults are typed model state, not values inferred by the YAML parser.
- Version and migration policy is declared by the semantic owner; automatic aggregate reflection is not a compatibility guarantee.
- A restore that can invalidate live cross-service state prepares and validates a candidate before changing the live authority.
- An owner marks the exact captured state durable only after encoding and file replacement report success; the atomic-replacement specification owns the platform crash-durability limit of that success.
- Path overrides change location, not schema ownership or lifecycle semantics.
- Authored configuration, runtime component state, and regenerable caches remain separate persistence classes even when they share YAML as an encoding.
- Exact schemas, paths, enum encodings, and version gates are delegated to reference rather than duplicated in architecture.

## Failure, cancellation, and lifetime boundaries

Persistence owners decide whether a failure is surfaced, retried, logged with a fallback, or blocks a lifecycle transition.
The grouped store and specialized-store specifications own their YAML containment and operational error mapping.
At the architecture boundary, file and I/O failures remain distinguishable from parse, node-kind, scalar, and schema rejection so the semantic owner can make that policy decision.
The [failure and reporting architecture](failure-and-reporting.md) owns how workflow owners retain, log, report, or present those failures.

The [LMDB operation specification](../spec/storage/lmdb-operation.md) owns the core environment, transaction, read/write, and operational-failure boundary.
Library database failures remain core/runtime library failures above that adapter, while frontend-owned preference stores may apply best-effort fallback according to their owning specification.

Ordinary `ConfigStore` work is synchronous and has no cancellation boundary.
Owners that schedule persistence asynchronously capture state under their own executor and must not move a shared mutable store to a worker while other clients continue to access it directly.
The [runtime execution architecture](runtime-execution.md) owns executor and teardown direction, and RFC 0005 tracks the proposed serialized asynchronous writer.

Before a runtime or library switch ends, owners that require a final checkpoint run while the referenced runtime state and stores still exist.
Playback-session shutdown precedes destruction of its sequence, transport, async runtime, and borrowed config store.
Frontend stores shared with windows or controllers outlive those consumers and are released after their final save opportunity.
The specialized layout component-state store provides its own mutex-protected operation lifetime; that property does not extend to `ConfigStore`.

## Implementation map

- [`MusicLibrary`](../../include/ao/library/MusicLibrary.h) owns durable per-library database state.
- [`RymlAdapter.h`](../../include/ao/yaml/RymlAdapter.h) contains RapidYAML callbacks, file reading, parsing, scalar conversion, and node helpers.
- [`AtomicFile.h`](../../include/ao/utility/AtomicFile.h), [`AtomicFile.cpp`](../../lib/utility/AtomicFile.cpp), and [`AtomicFileWindows.cpp`](../../lib/utility/AtomicFileWindows.cpp) provide platform file replacement.
- [`ConfigStore`](../../app/include/ao/rt/ConfigStore.h), [`ConfigStore.cpp`](../../app/runtime/ConfigStore.cpp), and [`ConfigTraits.h`](../../app/include/ao/yaml/ConfigTraits.h) implement the grouped runtime mechanism and common application codec.
- [`AppRuntimeDependencies`](../../app/include/ao/rt/AppRuntime.h) injects workspace and playback-session stores.
- [`WorkspaceService`](../../app/include/ao/rt/WorkspaceService.h) and [`WorkspaceService.cpp`](../../app/runtime/WorkspaceService.cpp) own workspace snapshot and restore coordination.
- [`PlaybackSessionPersistence`](../../app/runtime/PlaybackSessionPersistence.h) owns playback-session validation, scheduling, revision acknowledgement, and store use.
- [`AppConfigStore`](../../app/linux-gtk/app/AppConfigStore.h) owns the global GTK file boundary.
- [`KeymapStore`](../../app/include/ao/uimodel/input/KeymapStore.h), [`LayoutDocument`](../../app/include/ao/uimodel/layout/document/LayoutDocument.h), and UIModel presentation stores own platform-neutral state and encoding helpers.
- [`ShellLayoutStore`](../../app/linux-gtk/app/ShellLayoutStore.h), [`ShellLayoutComponentStateStore`](../../app/linux-gtk/app/ShellLayoutComponentStateStore.h), and [`GtkLayoutStateStore`](../../app/linux-gtk/app/GtkLayoutStateStore.h) are GTK file adapters.
- [`app/linux-gtk/main.cpp`](../../app/linux-gtk/main.cpp) and [`app/tui/Main.cpp`](../../app/tui/Main.cpp) select frontend paths and compose stores.

## Test map

- [`MusicLibraryTest.cpp`](../../test/unit/library/MusicLibraryTest.cpp) protects library-database ownership and lifetime.
- [`ConfigStoreTest.cpp`](../../test/unit/runtime/ConfigStoreTest.cpp) protects lazy load, grouped mutation, permissive decoding, read-only mode, failures, removal, and flush results.
- [`AtomicFileTest.cpp`](../../test/unit/utility/AtomicFileTest.cpp) protects replacement, permissions, and write-failure behavior below the stores.
- [`RymlAdapterTest.cpp`](../../test/unit/utility/RymlAdapterTest.cpp) protects strict scalar parsing, recoverable helpers, and callback diagnostic lifetime.
- [`WorkspaceSessionTest.cpp`](../../test/unit/runtime/WorkspaceSessionTest.cpp) protects workspace absence, restore rollback, and failure propagation.
- [`PlaybackSessionTest.cpp`](../../test/unit/runtime/PlaybackSessionTest.cpp) and [`PlaybackSessionRevisionTest.cpp`](../../test/unit/runtime/playback/PlaybackSessionRevisionTest.cpp) protect exact decoding, semantic validation, dirty revisions, retry, discard, and store selection.
- [`AppConfigStoreTest.cpp`](../../test/unit/linux-gtk/app/AppConfigStoreTest.cpp) and [`KeymapStoreTest.cpp`](../../test/unit/uimodel/input/KeymapStoreTest.cpp) protect global GTK groups and delta-from-default keymaps.
- [`ShellLayoutStoreTest.cpp`](../../test/unit/linux-gtk/app/ShellLayoutStoreTest.cpp), [`ShellLayoutComponentStateStoreTest.cpp`](../../test/unit/linux-gtk/app/ShellLayoutComponentStateStoreTest.cpp), and [`GtkLayoutStateStoreTest.cpp`](../../test/unit/linux-gtk/app/GtkLayoutStateStoreTest.cpp) protect the specialized GTK file boundaries.

## Related documents

- [System architecture](system-overview.md)
- [Runtime execution architecture](runtime-execution.md)
- [Failure and reporting architecture](failure-and-reporting.md)
- [Library architecture](library.md)
- [Playback architecture](playback.md)
- [Presentation architecture](presentation.md)
- [Application shell architecture](application-shell.md)
- [Workspace architecture](workspace.md)
- [Interactive session lifecycle architecture](interactive-session-lifecycle.md)
- [Reusable YAML adapter specification](../spec/persistence/yaml-adapter.md)
- [Grouped configuration store specification](../spec/persistence/config-store.md)
- [Atomic file replacement specification](../spec/persistence/atomic-replacement.md)
- [Managed file locations](../reference/persistence/location.md)
- [Application managed-state surface](../reference/persistence/application-config.md)
- [List presentation preference specification](../spec/presentation/list-preference.md)
- [LMDB operation specification](../spec/storage/lmdb-operation.md)
- [Library database reference](../reference/library/storage/database.md)
- [RFC 0005: coherent playback application boundary](../rfc/0005-coherent-playback-boundary.md), including the proposed serialized configuration writer
- [RFC 0010: versioned presentation state](../rfc/0010-versioned-presentation-state.md), including the proposed stable-id codec and migrations
- [RFC 0014: observable atomic replacement](../rfc/0014-observable-atomic-replacement.md), including proposed replacement receipts, security policy, cleanup, and fault injection
- [RFC 0015: fail-closed grouped configuration transactions](../rfc/0015-fail-closed-config-store.md), including candidate decoding, blocked-store recovery, and receipt-bearing document commits
- [RFC 0025: bounded shell layout documents](../rfc/0025-bounded-shell-layout-documents.md), including strict version dispatch, resource budgets, and unsupported-file preservation
- [Playback session persistence specification](../spec/playback/session-persistence.md) and [state reference](../reference/playback/session-state.md)
