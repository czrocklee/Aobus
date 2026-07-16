---
id: persistence.config-store
type: spec
status: current
domain: persistence
summary: Defines grouped managed-file loading, candidate decoding, atomic group saves and removals, failure, and concurrency behavior.
---
# Grouped configuration store

## Scope

This specification owns the current behavior of `ao::rt::ConfigStore` and the common application configuration traits it invokes: lazy whole-file loading, named top-level groups, ordinary and exact candidate decoding, one-shot durable group saves and removals, open modes, failure translation, and access serialization.

It does not own the meaning, defaults, validation, version, migration, restore policy, save trigger, retry policy, or reporting policy of any stored payload.
Those remain with the runtime, UIModel, or frontend component whose live behavior depends on the value.
It also does not own library YAML interchange, standalone layout documents, component-state documents, exact managed-file paths, or the platform guarantees defined by the [atomic file replacement specification](atomic-replacement.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) places `ConfigStore` in the application-runtime layer.
The [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md) assigns semantic policy to payload owners, grouped file mechanics to `ConfigStore`, reusable YAML and file-replacement mechanics to Core, and path selection to composition roots.

The public store is [`app/include/ao/rt/ConfigStore.h`](../../../app/include/ao/rt/ConfigStore.h); its non-template implementation is [`app/runtime/ConfigStore.cpp`](../../../app/runtime/ConfigStore.cpp).
The application codec is [`app/include/ao/yaml/ConfigTraits.h`](../../../app/include/ao/yaml/ConfigTraits.h).
It builds on the Core [YAML adapter](yaml-adapter.md) and [`AtomicFile`](../../../include/ao/utility/AtomicFile.h) mechanisms without making either mechanism specific to configuration.

`ConfigStore` may not depend on UIModel, a frontend, platform path discovery, or a payload-specific schema.
Direct users of the reusable YAML adapter outside `ConfigStore` retain their own format and failure contracts.

## Terminology

- The **backing file** is the one path supplied when the store is constructed.
- The **live document** is the complete successfully established YAML mapping cached by the store.
- A **group** is a named child of the live document's top-level mapping.
- A **seeded target** is the caller-provided object passed to a load operation, potentially already containing defaults or live values.
- A **decode candidate** is an isolated copy of the seeded target that receives ordinary or exact decoding before successful assignment.
- A **write candidate** is an isolated copy of the complete live document that receives every group replacement or removal for one operation.
- An **ordinary decode** applies the permissive application configuration traits through `load()`.
- An **exact decode** applies `readExact` through `loadExact()`; exactness is recursive for reflected aggregates and vectors, not a universal schema system for every supported type.

## Invariants

- One non-copyable, non-movable `ConfigStore` instance owns one backing path, one live YAML document, one parser input buffer, and one parser callback state.
- A successfully established document is cached for the remaining lifetime of the instance; the store does not merge or reload later external file changes.
- An existing backing file is established only after complete reading, YAML parsing, and top-level mapping validation succeed.
- File inspection, file reading, YAML parsing, or root validation failure does not install parser output or mark the document loaded, so a later operation retries initialization.
- Loading never writes the backing file, and destruction never saves automatically.
- Every present group decode occurs on a candidate; failure leaves the caller's seeded target unchanged.
- A save encodes every requested group on one complete write candidate and reaches file replacement only after initialization and encoding finish.
- A failed save or removal leaves both the live document and backing file unchanged under the atomic-replacement failure contract.
- A successful save or effective removal atomically replaces the complete backing file and only then installs the matching write candidate as the live document.
- One save may replace several groups in one whole-document commit.
- The public API exposes no bare flush, staged mutable tree, dirty bit, revision, receipt, reload, retry, or recovery operation.
- `ReadOnly` permits inspection and decoding but treats save and removal attempts as invariant faults.
- One instance has no internal synchronization; its owner confines all access to one executor or serializes it externally.
- Different instances targeting the same path do not coordinate and can replace one another's groups from stale whole-file documents.

## State model

The store has only a lazy-load state.
It does not expose a staged or dirty state because every public mutation either completes its whole-file replacement or leaves the live document unchanged.

| State | Document source | Next initialization attempt |
|---|---|---|
| Unloaded | Default-constructed in-memory tree. | `contains()`, `load()`, `loadExact()`, `save()`, and `removeGroup()` inspect and, when present, parse the backing file. |
| Loaded | A validated existing top-level mapping or a new empty mapping for a missing `ReadWrite` file. | No operation rereads the backing file. |

A failed inspection, read, parse, or top-level mapping check leaves the store Unloaded.
A missing backing file transitions a `ReadWrite` store to Loaded with an empty top-level mapping; the same condition returns `NotFound` and leaves a `ReadOnly` store Unloaded.

## Commands and transitions

### Construction and open modes

Construction captures the backing path and initializes parser callback state with an owned copy of that path for diagnostics.
It performs no file access.

`ReadWrite` is the default mode and permits a missing file to become a new empty document on the first initializing operation.
`ReadOnly` requires the file to exist when initialization is attempted.
The mode is fixed for the lifetime of the store.

### Lazy initialization

The first initializing operation inspects the backing path.
For an existing file, it reads the complete byte sequence into a local buffer, parses into a local tree, and validates a top-level mapping.
Only then does it install the buffer and tree as the live document.
An existing empty file is a non-mapping document and is rejected; only an absent `ReadWrite` file establishes a new empty mapping.

An external change to the backing file after successful initialization is invisible to that instance.
A later save can therefore replace that external change with the instance's cached whole-file document.

### Group inspection and loading

| Operation | Present readable group | Missing group | Initialization or decode failure |
|---|---|---|---|
| `contains(group)` | Returns `true`. | Returns `false`. | Returns the initialization error. |
| `load(group, target)` | Ordinary-decodes a candidate and assigns it to `target` after complete success. | Succeeds without changing `target`. | Returns the initialization or `FormatRejected` decode error and leaves `target` unchanged. |
| `loadExact(group, target)` | Exact-decodes a candidate and assigns it to `target` after complete success. | Succeeds without changing `target`. | Returns the initialization or `FormatRejected` decode error and leaves `target` unchanged. |

Group absence is a normal outcome rather than `NotFound`.
The caller cannot distinguish absence from a successfully accepted group by inspecting the `Result` from either load operation; it uses `contains()` when that distinction is required.
Both load operations require the target type to be copy-constructible and move-assignable so decoding can preserve the seeded target until the candidate succeeds.

### Ordinary decode

Ordinary decode is intended for seeded configuration values whose schema owner accepts permissive restore:

- Reflected aggregates require a mapping, visit known fields, preserve the seed for absent fields, and ignore unknown fields.
- An invalid present aggregate field rejects the decode candidate; fields visited earlier may have changed that candidate but never the caller's target.
- Vectors and string-keyed maps require the matching container node, clear the candidate container, skip children that do not decode, and accept the resulting container.
- Fixed arrays visit available sequence children in order, ignore excess children, and retain candidate elements when a child is invalid or absent.
- Invalid optional content clears the candidate optional and rejects the enclosing field.
- Arithmetic and boolean fields use the strict scalar conversions owned by the [reusable YAML adapter specification](yaml-adapter.md).
- Enums are stored as signed 32-bit numeric values and decoded by casting that value; the common trait does not validate membership in the enum's declared domain.
- Strong wrapper types supported through `raw()` use the codec for their underlying value.

Generic container salvage remains part of ordinary decoding.
A semantic owner that requires strict element rejection selects exact decoding where sufficient or supplies a payload-specific codec and validation boundary.

### Exact decode

Exact decode tightens reflected aggregates and vectors:

- An aggregate node must be a mapping with exactly the reflected field count, every reflected field must be present, and each field must recursively decode.
- A vector node must be a sequence and every element must recursively decode.
- The outer `ConfigStore` decode candidate prevents any failed exact operation from changing the caller's target.

For scalars, optionals, arrays, maps, and other types without a specialized `readExact` overload, exact decode delegates to ordinary decode.
`loadExact()` therefore does not by itself provide field aliases, schema versions, enum-domain validation, semantic validation, or universal strictness for every possible nested type.
Versioned payload owners add those policies above the store.

### Save

`save()` accepts one or more group/value pairs as one operation.
It establishes the live document, copies the complete document into a write candidate, and replaces each named group on that candidate in call order.
Unregistered sibling groups remain present.

Encoding and YAML emission happen before file replacement and do not mutate the live document.
An escaping codec, allocation, or emitter exception discards the candidate through stack unwinding and leaves the live document and backing file unchanged.

After successful emission, the store passes the complete candidate bytes to the [atomic file replacement contract](atomic-replacement.md).
A returned replacement failure leaves the live document unchanged.
Replacement success installs the already-built candidate through no-throw tree move assignment and returns success.

There is no public operation that persists an uninitialized or partially encoded live tree.

### Removal

`removeGroup(group)` establishes and copies the complete document.
If the group is absent, removal succeeds without creating or rewriting a file.
If the group is present, the operation removes it from the write candidate and uses the same whole-document replacement and live-install ordering as `save()`.
Repeated removal is therefore idempotent.

## Failure and cancellation

| Condition | Observable channel |
|---|---|
| Backing-path inspection or file read fails | `Result` with `IoError`. |
| Backing file is missing in `ReadWrite` mode | Successful initialization of an empty document. |
| Backing file is missing in `ReadOnly` mode | `Result` with `NotFound`. |
| YAML syntax parsing throws or the document root is not a mapping | `Result` with `FormatRejected` and backing-file context. |
| A present group returns false from ordinary or exact decode | `Result` with `FormatRejected` and group context. |
| A standard exception escapes group decoding | `Result` with `FormatRejected` and group context. |
| Atomic file replacement fails | The helper's recoverable failure, currently `IoError`; the live document remains unchanged. |
| `save()` or `removeGroup()` is called on a `ReadOnly` store | `ao::Exception` invariant fault. |

Encoding and YAML emission are not wrapped in a broad exception translation by `ConfigStore`.
An unexpected codec, allocation, or emitter exception remains an invariant fault while candidate isolation provides the strong no-change guarantee.

All store operations are synchronous and expose no cancellation point.
Scheduling, retry, coalescing, shutdown checkpoints, fallback, logging, notifications, and user presentation belong to the semantic owner and surrounding workflow.

## Persistence and versioning

Every effective mutation replaces one complete YAML file; group updates are not independently written.
The [atomic file replacement specification](atomic-replacement.md) owns temporary-file creation, permission application, write barriers, replacement, cleanup, and platform-specific guarantees.

`ConfigStore` defines no file-level schema identifier, schema version, group registry, field alias, migration, or compatibility window.
Group names and payload schemas belong to their semantic owners and exact serialized references.
Reflected C++ field names and numeric enum values are current encodings, not an automatic promise that renamed members or reordered enum definitions remain compatible.

The exact default and platform paths for managed files belong to the [managed file locations reference](../../reference/persistence/location.md).

## Frontend observations

`ConfigStore` publishes no event, revision, progress, activity, or notification.
A runtime or frontend workflow decides whether a store failure is local validation, retained status, retryable background work, a notification, or a logged fallback according to its owning specification.

## Implementation map

- [`ConfigStore.h`](../../../app/include/ao/rt/ConfigStore.h) owns the public modes, grouped save templates, candidate decoding, and codec invocation.
- [`ConfigStore.cpp`](../../../app/runtime/ConfigStore.cpp) owns lazy candidate parsing, document cloning, removal, emission, atomic replacement invocation, and live-document installation.
- [`ConfigTraits.h`](../../../app/include/ao/yaml/ConfigTraits.h) owns the common application codec and ordinary/exact distinction.
- [`RymlAdapter.h`](../../../include/ao/yaml/RymlAdapter.h) owns reusable parser callbacks, file-reading helpers, scalar conversion, and tree node helpers.
- [`AtomicFile.h`](../../../include/ao/utility/AtomicFile.h), [`AtomicFile.cpp`](../../../lib/utility/AtomicFile.cpp), and [`AtomicFileWindows.cpp`](../../../lib/utility/AtomicFileWindows.cpp) own the lower file-replacement mechanism.

## Test map

- [`ConfigStoreTest.cpp`](../../../test/unit/runtime/ConfigStoreTest.cpp) protects round trips, seeded defaults, group absence, scalar boundaries, multi-group commits, group overwrite and preservation, candidate string ownership, empty and rejected-document byte preservation, failed-encoding and failed-replacement isolation, failed-decode target isolation, removal, read-only behavior, and retry after initialization failure.
- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp) protects exact decode and semantic validation above the store.
- [`RymlAdapterTest.cpp`](../../../test/unit/utility/RymlAdapterTest.cpp) protects complete scalar parsing, range rejection, canonical booleans, recoverable file helpers, and callback diagnostic lifetime.
- [`AtomicFileTest.cpp`](../../../test/unit/utility/AtomicFileTest.cpp) protects replacement, owner-only permissions, and lower write-failure behavior.

## Related documents

- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [System architecture](../../architecture/system-overview.md)
- [Outcome channel specification](../failure/outcome-channel.md)
- [Reusable YAML adapter specification](yaml-adapter.md)
- [Atomic file replacement specification](atomic-replacement.md)
- [Managed file locations reference](../../reference/persistence/location.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
- [RFC 0005: coherent playback application boundary](../../rfc/0005-coherent-playback-boundary.md)
- [RFC 0010: versioned presentation state](../../rfc/0010-versioned-presentation-state.md)
- [RFC 0014: observable atomic replacement](../../rfc/0014-observable-atomic-replacement.md), rejected after narrower atomic-replacement hardening was implemented
- [RFC 0015: fail-closed grouped configuration transactions](../../rfc/0015-fail-closed-config-store.md), rejected after the narrower candidate-save boundary closed the verified destructive paths
- [Playback session persistence specification](../playback/session-persistence.md) and [state reference](../../reference/playback/session-state.md)
