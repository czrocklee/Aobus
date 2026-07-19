---
id: architecture.persistence-and-managed-state
type: architecture
status: current
domain: persistence
summary: Defines durable-data and managed-state ownership, serialization, storage, composition, and lifecycle boundaries.
---
# Persistence and managed-state architecture

## Scope

This document owns how Aobus classifies persisted state, assigns one authority to each persistence domain, and moves typed managed state through schemas, stores, paths, and lifecycle policy.
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
  -> explicit owner-local schema
  -> ConfigStore or a specialized file store
  -> ao::yaml parsing/emission + AtomicFile replacement
  -> canonical per-library path or platform path selected at composition
```

The second diagram shows the save direction.
Load traverses the same boundary in reverse until the semantic owner validates a candidate and decides whether it can replace live state.

The principal code boundaries refine the layer model in the [system architecture](system-overview.md):

| Concern | System layer | Public boundary | Implementation |
|---|---|---|---|
| Durable library data | Core libraries | `ao::library::MusicLibrary` | `include/ao/library/` and `lib/library/` over LMDB |
| YAML and atomic-file mechanisms | Core libraries | `include/ao/yaml/` and `include/ao/utility/AtomicFile.h` | Header adapters and `lib/utility/AtomicFile.cpp` |
| Grouped managed file | Application runtime | `app/include/ao/rt/ConfigStore.h` | `app/runtime/ConfigStore.cpp`; payload schemas remain beside their runtime, UIModel, or frontend owners |
| Canonical per-library paths | Application runtime | `app/include/ao/rt/library/LibraryPaths.h` | `app/runtime/library/LibraryPaths.cpp` |
| Runtime session semantics | Application runtime | `WorkspaceService`, `AppRuntime`, and playback-session commands | Runtime workspace and playback-persistence implementations under the [workspace](workspace.md), [interactive session lifecycle](interactive-session-lifecycle.md), and playback owners |
| Platform-neutral presentation state | UIModel | Input, layout, and library-presentation store/model headers | `app/uimodel/` model schemas and state helpers |
| Platform locations, overrides, and stores | GTK, TUI, and CLI frontends | Frontend-local composition and store adapters | `app/linux-gtk/app/`, `app/linux-gtk/main.cpp`, `app/tui/`, and `app/cli/` |

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
Persistence through a shared file mechanism does not transfer that responsibility to the file store.

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

### Schemas and schema policy

Every managed payload has an explicit schema beside its semantic or format owner.
That schema spells stable field names, enum and identifier representations, version dispatch, required and optional fields, unknown-key policy, structural rejection, and any semantic validation that must precede installation.
Its `deserialize` operation receives the owner's seeded value and returns a complete candidate; its `serialize` operation builds an owned YAML subtree.

`ConfigStore` requires a schema at every load and save call and never derives a schema from the C++ value type.
It has no reflected compatibility path, generic enum casting, `raw()` wrapper convention, or application serialization traits.
Core `ao::yaml` supplies only domain-neutral node-kind, map-key, explicitly directed map/sequence traversal, scalar, arena, parsing, and diagnostic helpers.
The separate Core reflection helper is a one-way output facility and cannot define a managed-state format that Aobus reads.
This boundary forbids implicit schema derivation from member layout; it does not preclude a future reflection-backed schema whose owner metadata explicitly fixes stable keys, representations, versions, defaults, and validation.

Workspace and playback keep their schemas in runtime; presentation and shell-layout formats keep theirs in UIModel; GTK global preferences keep their small schemas in the frontend adapter.
GTK presentation paths remain adapters over UIModel-owned schemas rather than becoming schema owners.

Authored shell-layout and component-state schemas reject unsupported versions before interpreting version-specific payload fields.
[RFC 0025](../rfc/0025-bounded-shell-layout-documents.md) still proposes shell-specific file, model, and template-expansion budgets; those resource limits are distinct from the explicit schema boundary.

The schema owner decides whether absence retains seeded defaults, whether unknown fields are tolerated, whether malformed nested data rejects the whole candidate, and whether an older representation can be migrated.
No compatibility or migration path exists merely because an earlier implementation reflected a C++ aggregate.

### Store and composition owners

`ConfigStore` is the application-runtime mechanism for multiple named groups that share one whole-file document and writer authority.
The [grouped configuration store specification](../spec/persistence/config-store.md) owns its lazy initialization, explicit schema invocation, presence-aware candidate loads, atomic multi-group saves and removals, failures, and concurrency contract.
The semantic owner above the store remains responsible for dirty state, scheduling, retry, observation, cross-field validation, fallback, and save acknowledgement.

A specialized store may bypass `ConfigStore` when its document boundary, synchronization, or pruning behavior differs from grouped application configuration.
`ShellLayoutComponentStateStore`, for example, reads and emits one complete component-state document directly, protects its operations with a mutex, and still uses the shared YAML and atomic-file mechanisms.

`LibraryPaths` derives the canonical managed-data base, database, and log locations from a supplied music root and recognizes whether the canonical database already exists.
It does not discover XDG, GLib, terminal, or command-line locations.

GTK and TUI select platform directories, a music root, and explicit overrides; use `LibraryPaths` for standard per-library locations; construct stores before `AppRuntime`; and keep any borrowed store alive until runtime persistence has shut down.
`AppRuntime` owns its workspace store and borrows an explicitly supplied playback-session store; when none is supplied, playback session and workspace use the same owned instance.

GTK supplies its global `AppConfigStore` as the playback-session store while supplying a per-library workspace store separately.
TUI currently uses its selected `ConfigStore` for both roles.
CLI opens the library database for the selected root but does not load interactive managed state.

## Boundaries and dependency direction

- Core storage, YAML, and atomic-file mechanisms cannot depend on runtime, UIModel, a frontend, or any application schema.
- Runtime persistence may depend on Core mechanisms and may derive canonical cross-frontend paths from a supplied music root, but it cannot depend on UIModel or discover platform application directories.
- UIModel may define typed serializable values and schemas and may use the runtime `ConfigStore` along the top-level dependency direction, but it cannot resolve GTK, terminal, or XDG locations.
- Frontends own platform locations, explicit overrides, frontend-specific filenames, and file adapters, but they do not duplicate the canonical per-library base, database path, log path, or LMDB existence marker.
- Cross-frontend runtime state remains owned and validated by runtime.
- A generic schema or store cannot become the semantic owner of a group merely because it names, serializes, or flushes that group.
- Library LMDB truth and library YAML interchange cannot be routed through `ConfigStore`; they retain their own storage and format owners.
- Each durable location has one application-level writer authority.
- `ConfigStore` has no internal synchronization contract; all access to one instance is confined to its owning executor or externally serialized.
- Multiple groups in one file share one mutable document and therefore one writer authority.
  Independent writers targeting the same path would each hold a stale whole-file tree and can overwrite one another's groups.
- A specialized store may define a different synchronization contract, but that contract remains local to the specialized type.

## Data and control flow

### Composition and restore

```text
frontend selects platform directories, music root, and overrides
  -> LibraryPaths derives canonical per-library locations when no override applies
  -> constructs owned and borrowed stores
  -> constructs CoreRuntime or AppRuntime
  -> store or specialized file adapter establishes its managed document
  -> owner selects its document or top-level group
  -> schema deserializes into a seeded value or separate candidate
  -> semantic owner validates identities, versions, and cross-field invariants
  -> owner commits, normalizes, falls back, or retains current/default state
```

Missing data is not automatically an error or automatically a default.
The semantic owner decides whether absence means first run, no customization, no restorable session, or a required document failure.

The store invokes exactly the schema supplied by the owner and installs only its complete returned candidate.
Its presence result distinguishes a missing group from an accepted group without assigning defaults on the owner's behalf.
Workspace, playback, presentation, layout, component-state, keymap, and GTK preference schemas each define their own recursive strictness and candidate semantics.

### Save

```text
semantic owner captures one coherent typed value
  -> model schema serializes the owned group or document
  -> store applies all requested groups to an isolated complete-document candidate
  -> store emits the complete candidate
  -> AtomicFile replaces the previous file
  -> store installs the matching candidate as its live document
  -> semantic owner acknowledges only the successfully replaced snapshot
```

The diagram's atomic replacement and live-candidate installation form one synchronous `ConfigStore` save operation; semantic owners do not stage a mutable tree or invoke a separate flush.

Atomic replacement protects the target path from a helper-written partial replacement under the [platform replacement contract](../spec/persistence/atomic-replacement.md).
It always installs a private-user file after a complete write, data barrier, and close, and gives every uncommitted temporary file one RAII cleanup owner.
Its success means the platform replacement call succeeded; it does not serialize concurrent writers, prove absolute power-loss durability, or acknowledge a newer in-memory revision on behalf of the state owner.

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
- Version and migration policy is declared by the semantic owner; managed-state schemas are never derived solely from unannotated aggregate reflection or member layout.
- A restore that can invalidate live cross-service state prepares and validates a candidate before changing the live authority.
- An owner settles the exact captured state only after serialization and file replacement report success; the atomic-replacement specification owns the platform crash-durability limit of that acknowledgement.
- Path overrides change location, not schema ownership or lifecycle semantics.
- Canonical per-library paths have one runtime authority; frontend-specific files may extend the managed-data base without redefining that base.
- Authored configuration, runtime component state, and regenerable caches remain separate persistence classes even when they share YAML as a serialization format.
- Exact schemas, paths, enum encodings, and version gates are delegated to reference rather than duplicated in architecture.

## Failure, cancellation, and lifetime boundaries

Persistence owners decide whether a failure is surfaced, retried, logged with a fallback, or blocks a lifecycle transition.
The grouped store and specialized-store specifications own their YAML containment and operational error mapping.
At the architecture boundary, file and I/O failures remain distinguishable from parse, node-kind, scalar, and schema rejection so the semantic owner can make that policy decision.
The [failure and reporting architecture](failure-and-reporting.md) owns how workflow owners retain, log, report, or present those failures.

The [LMDB operation specification](../spec/storage/lmdb-operation.md) owns the core environment, transaction, read/write, and operational-failure boundary.
Library database failures remain core/runtime library failures above that adapter, while frontend-owned preference stores may apply best-effort fallback according to their owning specification.

`ConfigStore` work is synchronous and has no cancellation boundary.
Owners that schedule persistence asynchronously capture state under their own executor and must not move a shared mutable store to a worker while other clients continue to access it directly.
The [runtime execution architecture](runtime-execution.md) owns executor and teardown direction, and RFC 0005 tracks the proposed serialized asynchronous writer.

Before a runtime or library switch ends, owners that require a final checkpoint run while the referenced runtime state and stores still exist.
Playback-session shutdown precedes destruction of its sequence, transport, async runtime, and borrowed config store.
Frontend stores shared with windows or controllers outlive those consumers and are released after their final save opportunity.
The specialized layout component-state store provides its own mutex-protected operation lifetime; that property does not extend to `ConfigStore`.

## Implementation map

- [`MusicLibrary`](../../include/ao/library/MusicLibrary.h) owns durable per-library database state.
- [`RymlAdapter.h`](../../include/ao/yaml/RymlAdapter.h) contains RapidYAML callbacks, file reading, parsing, arena lifetime helpers, and scalar conversion; [`Serialization.h`](../../include/ao/yaml/Serialization.h) contains the domain-neutral explicit map/sequence serialization helpers.
- [`AtomicFile.h`](../../include/ao/utility/AtomicFile.h), [`AtomicFileTransaction.h`](../../lib/utility/AtomicFileTransaction.h), [`AtomicFile.cpp`](../../lib/utility/AtomicFile.cpp), and [`AtomicFileWindows.cpp`](../../lib/utility/AtomicFileWindows.cpp) provide the private-file replacement state machine and platform operations.
- [`ConfigStore`](../../app/include/ao/rt/ConfigStore.h) and [`ConfigStore.cpp`](../../app/runtime/ConfigStore.cpp) implement the schema-neutral grouped runtime mechanism and explicit schema boundary.
- [`LibraryPaths`](../../app/include/ao/rt/library/LibraryPaths.h) and [`LibraryPaths.cpp`](../../app/runtime/library/LibraryPaths.cpp) own canonical per-library path derivation and existing-database detection.
- [`AppRuntimeDependencies`](../../app/include/ao/rt/AppRuntime.h) injects workspace and playback-session stores.
- [`WorkspaceService`](../../app/include/ao/rt/WorkspaceService.h) and [`WorkspaceService.cpp`](../../app/runtime/WorkspaceService.cpp) own workspace snapshot and restore coordination.
- [`WorkspaceSessionYamlSchema`](../../app/runtime/WorkspaceSessionYamlSchema.h) owns the strict workspace persistence DTO and stable presentation conversion.
- [`PlaybackSessionYamlSchema`](../../app/runtime/PlaybackSessionYamlSchema.h) owns playback-session structural and semantic candidate validation; [`PlaybackSessionPersistence`](../../app/runtime/PlaybackSessionPersistence.h) owns scheduling, revision acknowledgement, restore, and store use.
- [`AppConfigStore`](../../app/linux-gtk/app/AppConfigStore.h) owns the global GTK file boundary.
- [`KeymapStore`](../../app/include/ao/uimodel/input/KeymapStore.h), [`LayoutDocument`](../../app/include/ao/uimodel/layout/document/LayoutDocument.h), and the UIModel presentation schemas own platform-neutral state and serialization helpers.
- [`ShellLayoutStore`](../../app/linux-gtk/app/ShellLayoutStore.h), [`ShellLayoutComponentStateStore`](../../app/linux-gtk/app/ShellLayoutComponentStateStore.h), and [`GtkLayoutStateStore`](../../app/linux-gtk/app/GtkLayoutStateStore.h) are GTK file adapters.
- [`app/linux-gtk/main.cpp`](../../app/linux-gtk/main.cpp), [`app/tui/Main.cpp`](../../app/tui/Main.cpp), and [`CliRuntime.cpp`](../../app/cli/CliRuntime.cpp) select roots, platform locations, and overrides before composing runtime paths and stores.

## Test map

- [`MusicLibraryTest.cpp`](../../test/unit/library/MusicLibraryTest.cpp) protects library-database ownership and lifetime.
- [`ConfigStoreTest.cpp`](../../test/unit/runtime/ConfigStoreTest.cpp) protects lazy load, candidate deserialization, multi-group saves, rejected-input preservation, read-only mode, failures, and durable removal.
- [`LibraryPathsTest.cpp`](../../test/unit/runtime/library/LibraryPathsTest.cpp) protects exact canonical derivation and semantic existing-database detection through a real `MusicLibrary`.
- [`AtomicFileTest.cpp`](../../test/unit/utility/AtomicFileTest.cpp) protects replacement, cross-platform private-file policy, opaque payloads, and deterministic pre-replacement failure and cleanup behavior below the stores.
- [`RymlAdapterTest.cpp`](../../test/unit/utility/RymlAdapterTest.cpp) protects strict parsing, scalar conversion, and callback diagnostic lifetime; [`YamlSerializationTest.cpp`](../../test/unit/utility/YamlSerializationTest.cpp) protects explicit map/sequence composition, failure order, and arena ownership.
- [`WorkspaceSessionTest.cpp`](../../test/unit/runtime/WorkspaceSessionTest.cpp) protects workspace absence, restore rollback, and failure propagation.
- [`WorkspaceSessionYamlSchemaTest.cpp`](../../test/unit/runtime/WorkspaceSessionYamlSchemaTest.cpp) protects stable workspace presentation conversion and strict semantic rejection.
- [`PlaybackSessionTest.cpp`](../../test/unit/runtime/PlaybackSessionTest.cpp) and [`PlaybackSessionRevisionTest.cpp`](../../test/unit/runtime/playback/PlaybackSessionRevisionTest.cpp) protect exact deserialization, semantic validation, dirty revisions, retry, discard, and store selection.
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
- [Managed-state schema development guide](../development/managed-state-schemas.md)
- [Atomic file replacement specification](../spec/persistence/atomic-replacement.md)
- [Managed file locations](../reference/persistence/location.md)
- [Application managed-state surface](../reference/persistence/application-config.md)
- [List presentation preference specification](../spec/presentation/list-preference.md)
- [LMDB operation specification](../spec/storage/lmdb-operation.md)
- [Library database reference](../reference/library/storage/database.md)
- [RFC 0005: coherent playback application boundary](../rfc/0005-coherent-playback-boundary.md), including the proposed serialized configuration writer
- [RFC 0010: versioned presentation state](../rfc/0010-versioned-presentation-state.md), implemented with layered strict schemas, stable ids, and no legacy numeric migration
- [RFC 0014: observable atomic replacement](../rfc/0014-observable-atomic-replacement.md), rejected after narrower private-file, RAII-cleanup, and fault-test hardening was implemented
- [RFC 0015: fail-closed grouped configuration transactions](../rfc/0015-fail-closed-config-store.md), rejected after a narrower candidate-save boundary removed the destructive split API without blocked-store recovery or commit receipts
- [RFC 0025: bounded shell layout documents](../rfc/0025-bounded-shell-layout-documents.md), including strict version dispatch, resource budgets, and unsupported-file preservation
- [RFC 0032: explicit managed-state schemas](../rfc/0032-explicit-managed-state-schemas.md), implemented by the owner-local schemas and schema-neutral Core/store boundaries
- [Playback session persistence specification](../spec/playback/session-persistence.md) and [state reference](../reference/playback/session-state.md)
