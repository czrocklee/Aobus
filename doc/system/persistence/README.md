---
id: architecture.persistence-and-managed-state
---
# Persistence and managed-state architecture

Aobus does not have one universal persistence service.
Persisted data follows the component that owns its meaning, while reusable mechanisms provide transactions, YAML trees, grouped files, and atomic file replacement.

Use this page to decide which owner and mechanism a change belongs to.
Follow the linked topic contracts and references for exact behavior, paths, groups, formats, and versions.

## Choose the contract

- Durable music-library truth: [library architecture](../library/structure.md), [library database reference](../../reference/library/storage/database.md), and [LMDB operation specification](lmdb-operation.md)
- Grouped application-managed YAML: [grouped configuration store](config-store.md)
- YAML parsing, scalar conversion, and arena lifetime: [reusable YAML adapter](yaml-adapter.md)
- File replacement, permissions, cleanup, and durability limits: [atomic file replacement](atomic-replacement.md)
- Exact managed paths and overrides: [managed file locations](../../reference/persistence/location.md)
- Registered groups, payload owners, and current versions: [application managed-state surface](../../reference/persistence/application-config.md)
- Workspace restore and save: [workspace architecture](../workspace/README.md)
- Playback-session lifecycle: [playback session persistence](../playback/session-persistence.md)
- Presentation preferences: [presentation architecture](../presentation/README.md)
- Shell layout and component state: [application shell architecture](../shell/README.md)
- User-selected library YAML: [library YAML format](../../reference/library/format/yaml.md) and [transfer specification](../library/yaml-transfer.md)

## Persistence model

The main persistence classes have different authority and recovery expectations:

| Class | Examples | Authority |
|---|---|---|
| Durable library truth | Tracks, lists, resources, file identity, and library metadata | [`ao::library::MusicLibrary` over LMDB](../library/structure.md) |
| Managed application state | Workspace, playback intent, preferences, shortcuts, and presentation choices | [The runtime, UIModel, or frontend component whose live behavior uses the value](../../reference/persistence/application-config.md) |
| Authored documents | Customized shell layouts | The [layout document model](../../reference/shell/layout-document.md) and [shell workflow](../shell/layout-lifecycle.md) |
| Transient component state | Per-layout split and collapsible-panel state | The [component-state model and specialized store](../../reference/shell/layout-state.md) |
| Interchange | User-selected library YAML imports and exports | The [library format](../../reference/library/format/yaml.md) and [transfer workflow](../library/yaml-transfer.md), not `ConfigStore` |
| Regenerable output | [Cover caches](../resource/cover-art-delivery.md), [MPRIS artwork](../frontend/mpris.md), and [logs](../../reference/persistence/location.md) | The producing subsystem; these are not application truth |

Deleting a cache must not change library or managed-state facts.
It can still change what is immediately displayable: cached cover bytes may be the only remaining copy after every carrying audio file is gone.

Managed state normally flows through this boundary:

```text
semantic owner
  -> coherent typed candidate or immutable snapshot
  -> explicit owner-local schema
  -> ConfigStore or specialized store
  -> ao::yaml helpers
  -> AtomicFile replacement
  -> path selected by the composition root
```

Load follows the boundary in reverse.
Parsing a document does not make it usable: the schema and semantic owner must validate a complete candidate before replacing live state.

## Ownership

The component whose behavior depends on a value owns:

- the typed model and defaults;
- stable schema names and representations;
- version and migration policy;
- semantic and cross-field validation;
- the point at which a restored candidate becomes live; and
- save triggers, acknowledgement, retry, and user reporting.

A schema or file store does not gain semantic ownership merely because it serializes a value.
`ao::yaml` is a domain-neutral tree adapter.
`ConfigStore` is a schema-neutral grouped-file mechanism.
Neither discovers application schemas, enum mappings, defaults, or platform paths.

Current ownership is divided as follows:

- `MusicLibrary` exclusively owns durable library records.
- Runtime owns workspace and playback-session semantics.
- UIModel owns platform-neutral shortcut, presentation, and layout payloads.
- Frontend workflows own frontend-only preferences and save/restore lifecycle.
- Composition roots select platform directories, music roots, overrides, and concrete store sharing.
- `LibraryPaths` derives canonical per-library managed-data, database, and log locations from a supplied music root; it does not discover platform application directories.

Global state may contain library identities only when its lifecycle explicitly pairs the payload with the active library and validates those identities before restore.
Unrelated global preferences must not retain them.

## Schema boundary

Every managed payload uses an explicit schema beside its semantic or format owner.
The schema fixes field names, representations, required and optional values, unknown-key policy, version dispatch, structural rejection, and any validation required before installation.
Its deserialize operation receives an owner-supplied seed and returns a separate complete candidate.

`ConfigStore` requires that schema at each load or save call.
It has no reflected compatibility path, generic enum casting, application serialization trait, or implicit schema selection.
The Core reflection helper is one-way output and is not a managed-state reader.

Missing fields, unknown fields, unsupported versions, and malformed nested values are payload decisions, not global YAML policy.
A compatibility path exists only when its owner specifies one.

## Store and writer boundaries

`ConfigStore` owns one lazily established top-level YAML mapping and replaces the complete file for an effective mutation.
Several groups may share one store only when they also share one writer authority and lifecycle.
An instance caches its first accepted document and does not merge later external changes.
Two instances targeting the same file can therefore overwrite each other's groups from stale snapshots.

`ConfigStore` has no internal synchronization.
Its owner must confine one instance to an executor or serialize access externally.
It performs synchronous work and exposes no cancellation point.

A specialized store is appropriate when the document boundary or synchronization contract differs.
For example, shell component state is one complete document with a store-local mutex, while shell layout presets use bounded `ConfigStore` files and layout-specific preparation.
Such a specialized contract does not extend to other stores.

Each durable target has one application-level writer authority.
A parent-spawned desktop restart must release the old authority before launching its successor; independently launched processes are not serialized by the persistence mechanisms.

## Restore and save boundaries

Restore has three distinct stages:

1. The store or file adapter establishes a syntactically acceptable document.
2. The explicit schema returns an isolated typed candidate.
3. The semantic owner validates identities and cross-object invariants, then commits, normalizes, falls back, or retains current state.

Absence is not globally an error or a default.
The owner decides whether it means first run, no customization, no restorable session, or a required-document failure.

A grouped save follows this order:

1. The owner captures one coherent value.
2. Each schema serializes into an isolated copy of the complete document.
3. The store emits and atomically replaces that complete candidate.
4. Only after successful replacement does the store install the matching live document.
5. The semantic owner acknowledges only the snapshot that actually succeeded.

Atomic replacement prevents the helper from intentionally exposing a partially written target.
It does not serialize writers, create a multi-file transaction, provide recovery generations, or guarantee survival across every power loss and filesystem.
Those exact limits belong to the [atomic replacement specification](atomic-replacement.md).

## Composition and lifetime

GTK, TUI, WinUI, and CLI compose different subsets of the persistence model.
The exact files are listed in the [location reference](../../reference/persistence/location.md); the exact groups and versions are listed in the [application managed-state reference](../../reference/persistence/application-config.md).

Important composition rules are:

- Runtime never discovers platform application directories.
- Frontends do not duplicate canonical per-library database or managed-data derivation.
- GTK uses separate per-library workspace state and global application state; its global store is also the injected playback-session store.
- TUI keeps global preferences and shortcuts separate from its selected workspace/playback file and from per-library terminal presentation state.
- WinUI keeps desktop settings and playback in independent global files, while database, workspace, and presentation state follow the selected library root.
- CLI opens the library database and derived cache but does not load interactive managed state.
- A borrowed playback-session store must outlive runtime persistence shutdown.

Before replacing or destroying a runtime graph, owners run required final checkpoints while the referenced state and stores still exist.
Playback-session shutdown precedes destruction of its sequence, transport, asynchronous runtime, and borrowed store.
Frontend stores outlive the windows or controllers that receive their final save opportunity.

Desktop library switching has an additional admission boundary: the old playback payload is removed and the old writer graph is released before a successor starts; the successor admits new playback persistence only after its selected root is durably accepted.
The [desktop lifecycle specification](../desktop-library-lifecycle.md) and [interactive session lifecycle architecture](../session-lifecycle.md) own that workflow.

## Failure boundary

Persistence mechanisms preserve distinct failure categories so owners can choose fallback, retry, reporting, or transition blocking:

- file inspection and I/O failures;
- YAML syntax, node-kind, scalar, and schema rejection;
- unsupported versions;
- semantic candidate rejection; and
- LMDB capacity, conflict, stale-map, and operational failures.

Do not rewrite a normative transaction, replacement, or lifetime guarantee to match an implementation defect.
Report the disagreement at the owning boundary.
The [failure and reporting architecture](../failure/README.md) owns presentation and retention policy; the linked persistence contracts own their exact result and fatal channels.

## Dependency direction

- Core LMDB, YAML, and atomic-file mechanisms cannot depend on Runtime, UIModel, frontends, or application schemas.
- Runtime may use Core mechanisms and derive paths from a supplied music root, but it cannot discover platform application directories.
- UIModel may define serializable platform-neutral values and schemas, but it cannot resolve GTK, terminal, Windows, or XDG locations.
- Frontends own platform locations, explicit overrides, and frontend-specific file adapters, but not cross-frontend runtime semantics.
- Library LMDB truth and library YAML interchange never pass through `ConfigStore`.

These boundaries keep one semantic owner, one writer authority, and one compatibility contract for each persisted value without forcing unrelated persistence domains behind one facade.
