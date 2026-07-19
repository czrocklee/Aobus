---
id: persistence.config-store
type: spec
status: current
domain: persistence
summary: Defines schema-explicit grouped managed-file loading, optional byte ceilings, atomic group saves and removals, failure, and concurrency behavior.
---
# Grouped configuration store

## Scope

This specification owns the current behavior of `ao::rt::ConfigStore`: lazy whole-file loading, an optional whole-file byte ceiling, named top-level groups, explicit schema invocation, presence-aware candidate loading, one-shot multi-group saves and removals, open modes, failure translation, and access serialization.

It does not own a payload schema, field name, default, identifier serialization, enum mapping, version, unknown-field policy, migration, semantic validation, restore policy, save trigger, retry policy, or reporting policy.
Those decisions remain with the runtime, UIModel, or frontend owner whose live behavior depends on the value.
It also does not own library YAML interchange, standalone layout documents, component-state documents, exact managed-file paths, or the platform guarantees defined by the [atomic file replacement specification](atomic-replacement.md).

## Code boundary

The [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md) assigns grouped file mechanics to `ConfigStore`, reusable YAML and file-replacement mechanics to Core, schema and semantic policy to payload owners, and path selection to composition roots.

The public store and `ConfigSchema` concept are in [`app/include/ao/rt/ConfigStore.h`](../../../app/include/ao/rt/ConfigStore.h); non-template document mechanics are in [`app/runtime/ConfigStore.cpp`](../../../app/runtime/ConfigStore.cpp).
The store builds on the Core [YAML adapter](yaml-adapter.md) and [`AtomicFile`](../../../include/ao/utility/AtomicFile.h) without making either mechanism specific to configuration.

`ConfigStore` may not depend on UIModel, a frontend, platform path discovery, or a payload-specific schema.
It does not transitively select or export an application serialization trait.

## Terminology

- The **backing file** is the one path supplied at construction.
- The **live document** is the complete successfully established top-level YAML mapping cached by the store.
- A **group** is a named direct child of the live document.
- A **seeded value** is the caller-provided value passed to a schema during load; its owner may use it to retain defaults for absent fields.
- A **deserialized value** is the isolated value returned by a schema after complete acceptance.
- A **config write** is a short-lived `configWrite(group, value, schema)` descriptor used to assemble one atomic multi-group operation.
- A **write candidate** is an isolated copy of the complete live document into which every schema for one save operation serializes its group.
- The optional **file-byte ceiling** is a caller-selected maximum for both an existing backing file and emitted replacement bytes.

For a value type `T`, a conforming schema provides these const operations:

```cpp
Result<> serialize(ryml::NodeRef output, T const& value) const;
Result<T> deserialize(ryml::ConstNodeRef input, T const& seed) const;
```

The payload owner decides whether `deserialize()` uses the seed, requires every field, rejects unknown keys, validates enum membership, dispatches a version before payload interpretation, or performs semantic validation.

## Invariants

- One non-copyable, non-movable store owns one backing path, live tree, parser input buffer, and parser callback state.
- A successfully established document is cached for the rest of the instance lifetime; the store does not merge or reload later external changes.
- An existing file is established only after complete reading, YAML parsing, top-level mapping validation, and rejection of duplicate or keyless group entries.
- When configured, the file-byte ceiling is checked from the existing file size before allocating its input buffer or parsing it.
- Failed inspection, reading, parsing, or root validation installs no parser output and leaves initialization retryable.
- Loading never writes, and destruction never saves.
- A present group is installed into the caller's value only after its schema returns a complete deserialized value.
- A missing group is reported distinctly and leaves the seeded value unchanged.
- Every group in a save is serialized directly into one private complete-document candidate; any schema failure discards that candidate.
- Schema-written dynamic keys and values are copied into the candidate tree's arena; no committed node borrows schema or caller storage.
- Serialization, candidate assembly, emission, replacement, or removal failure leaves both the live document and backing file unchanged under the atomic-replacement contract.
- When configured, emitted replacement bytes must fit the same file-byte ceiling before atomic replacement.
- Successful save or effective removal replaces the complete backing file and only then installs the matching candidate as the live document.
- One save may replace several groups in one whole-document commit; untouched sibling groups remain present.
- The API exposes no reflection-derived schema, implicit schema selection, staged mutable tree, flush, dirty bit, revision, receipt, reload, retry, or recovery operation.
- `ReadOnly` permits inspection and loading but treats save and removal as invariant faults.
- One instance has no internal synchronization; its owner confines access to one executor or serializes it externally.
- Different instances targeting the same path do not coordinate and may overwrite one another from stale whole-file documents.

## State model

The store has only lazy-load state; every public mutation either completes its whole-file replacement or leaves the live document unchanged.

| State | Document source | Next initialization attempt |
|---|---|---|
| Unloaded | Default-constructed in-memory tree. | `contains()`, `load()`, `save()`, and `removeGroup()` inspect and, when present, parse the backing file. |
| Loaded | A validated existing top-level mapping or a new empty mapping for a missing `ReadWrite` file. | No operation rereads the backing file. |

A missing backing file transitions a `ReadWrite` store to Loaded with an empty mapping.
The same condition returns `NotFound` and leaves a `ReadOnly` store Unloaded.

## Commands and transitions

### Construction and open modes

Construction captures the backing path and an owned diagnostic filename but performs no file access.
`ReadWrite` is the default and permits a missing file to become an empty document on first initialization.
`ReadOnly` requires the file to exist when initialization is attempted.
The mode and optional file-byte ceiling are fixed for the store lifetime.

### Lazy initialization

The first initializing operation inspects the backing path.
For an existing file, it rejects a configured byte-ceiling violation before allocating the candidate buffer, then reads all bytes, parses into a candidate tree, requires a top-level mapping, and rejects duplicate or keyless group entries while retaining unknown unique groups.
Only complete success installs the buffer and tree.
An existing empty file is rejected as a non-mapping document; only an absent `ReadWrite` file establishes an empty mapping.

An external file change after initialization is invisible to that store instance.
A later save may therefore replace the external change with the cached whole-file document.

### Group inspection and loading

| Operation | Present accepted group | Missing group | Initialization or deserialize failure |
|---|---|---|---|
| `contains(group)` | Returns `true`. | Returns `false`. | Returns the initialization error. |
| `load(group, value, schema)` | Calls `schema.deserialize(node, value)`, move-assigns its returned value, and returns `true`. | Returns `false` without invoking the schema or changing `value`. | Returns the initialization or contextualized schema error and leaves `value` unchanged. |

`load()` returns `Result<bool>` so callers do not need a separate `contains()` call to distinguish absence.
The value type must be move-assignable.
The store passes the caller's current value to the schema as a const seed; it never mutates that value during deserialization.

The schema's returned error code is preserved.
For example, a schema may return `FormatRejected` for malformed structure, `NotSupported` for an unsupported future version, or another domain-appropriate recoverable error.
The store adds bounded group and operation context to the message.

### Save

`save()` replaces one group.
`saveTogether()` accepts two or more explicit config-write descriptors as one operation:

```cpp
store.save("first", firstValue, firstSchema);

store.saveTogether(configWrite("first", firstValue, firstSchema),
                   configWrite("second", secondValue, secondSchema));
```

The operation proceeds in this order:

1. Establish and copy the complete live document into a write candidate.
2. For each descriptor in call order, remove that candidate group, create a replacement child, and call its schema exactly once.
3. Stop and discard the complete candidate if any schema fails; the private candidate may be partially assembled but is never externally visible.
4. Retain every unmentioned sibling and every earlier successfully serialized group on the candidate.
5. Emit the complete candidate and, when configured, reject bytes beyond the file-byte ceiling.
6. Atomically replace the backing file.
7. Move the candidate into the live document only after replacement succeeds.

Repeated group names in one `saveTogether()` call follow descriptor order, so the last supplied replacement is the resulting group.
There is no public operation that persists a partially serialized live tree.

The store does not infer a group shape from `T`.
The schema must explicitly establish the node kind and serialized content it owns.

### Removal

`removeGroup(group)` establishes and copies the complete document.
If the group is absent, it succeeds without creating or rewriting a file.
If present, it removes the group on the candidate and uses the same complete-document replacement and live-install ordering as save.
Repeated removal is idempotent.

## Failure and cancellation

| Condition | Observable channel |
|---|---|
| Backing-path inspection or complete-file read fails | `Result` with `IoError`. |
| Backing file is missing in `ReadWrite` mode | Successful initialization of an empty document. |
| Backing file is missing in `ReadOnly` mode | `Result` with `NotFound`. |
| Existing or emitted complete file exceeds the configured byte ceiling | `Result` with `ValueTooLarge`; the live document and backing file remain unchanged. |
| YAML syntax parsing throws or the root is not a mapping | `Result` with `FormatRejected` and backing-file context. |
| A schema returns an error | That error code with bounded group/operation context. |
| A standard exception escapes deserialization | `Result` with `FormatRejected`; the seeded value remains unchanged. |
| A standard exception escapes serialization, candidate assembly, or emission | `Result` with `InvalidState`; the live document and backing file remain unchanged. |
| Atomic replacement fails | The helper's recoverable error, currently `IoError`; the live document remains unchanged. |
| Save or removal is called on a `ReadOnly` store | `ao::Exception` invariant fault. |

Candidate preparation and non-standard exceptions are not broadly translated.
All operations are synchronous and expose no cancellation point.
Scheduling, retry, coalescing, checkpoints, fallback, logging, and user presentation belong to the owner above the store.

## Persistence and versioning

Every effective mutation replaces one complete YAML file; group updates are not independently written.
The [atomic file replacement specification](atomic-replacement.md) owns temporary files, permissions, write barriers, replacement, cleanup, and platform-specific guarantees.

`ConfigStore` defines no file-level schema identifier, schema version, group registry, field alias, enum representation, migration, or compatibility window.
Group names and payload schemas belong to their semantic owners and exact serialized references.
The store has no reflected read path or compatibility fallback.

## Frontend observations

The store publishes no event, revision, progress, activity, or notification.
A runtime or frontend workflow decides how to report or recover from a store failure according to its owning specification.

## Implementation map

- [`ConfigStore.h`](../../../app/include/ao/rt/ConfigStore.h) owns the public modes, `ConfigSchema` concept, `ConfigWrite` descriptors, explicit load/save templates, candidate serialization, schema exception containment, and error context.
- [`ConfigStore.cpp`](../../../app/runtime/ConfigStore.cpp) owns lazy parsing, complete-document cloning, removal, emission, atomic replacement, and live-document installation.
- [`RymlAdapter.h`](../../../include/ao/yaml/RymlAdapter.h) owns reusable parser callbacks, file reading, arena lifetime helpers, and scalar conversion; [`Serialization.h`](../../../include/ao/yaml/Serialization.h) owns explicit node validation and arena-owning map/sequence writes.
- [`AtomicFile.h`](../../../include/ao/utility/AtomicFile.h), [`AtomicFile.cpp`](../../../lib/utility/AtomicFile.cpp), and [`AtomicFileWindows.cpp`](../../../lib/utility/AtomicFileWindows.cpp) own the lower file-replacement mechanism.

## Test map

- [`ConfigStoreTest.cpp`](../../../test/unit/runtime/ConfigStoreTest.cpp) protects explicit schema round trips, presence results, seeded values, byte ceilings, failed-deserialize isolation, multi-group atomicity, exception translation, replacement failure, temporary-string ownership, removal, and read-only behavior.
- [`RymlAdapterTest.cpp`](../../../test/unit/utility/RymlAdapterTest.cpp) protects strict scalar parsing and parser diagnostics; [`YamlSerializationTest.cpp`](../../../test/unit/utility/YamlSerializationTest.cpp) protects node-kind and key validation, duplicate detection, unknown-key policy, bounded context, failure order, and arena-owned writes.
- Payload schema tests protect field vocabulary, versions, unknown and missing fields, enum membership, semantic rejection, and representative documents at their owning layers.
- [`AtomicFileTest.cpp`](../../../test/unit/utility/AtomicFileTest.cpp) protects replacement, owner-only permissions, and lower write-failure behavior.

## Related documents

- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [Reusable YAML adapter specification](yaml-adapter.md)
- [Atomic file replacement specification](atomic-replacement.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
- [Managed-state schema development guide](../../development/managed-state-schemas.md)
- [Managed file locations reference](../../reference/persistence/location.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [RFC 0015: fail-closed grouped configuration transactions](../../rfc/0015-fail-closed-config-store.md), rejected after candidate installation and complete-document saves closed the verified destructive paths
- [RFC 0032: explicit managed-state schemas](../../rfc/0032-explicit-managed-state-schemas.md), implemented by the explicit owner-schema boundary specified here
