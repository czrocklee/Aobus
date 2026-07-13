---
id: persistence.config-store
type: spec
status: current
domain: persistence
summary: Defines grouped managed-file loading, decoding, mutation, flushing, failure, and concurrency behavior.
---
# Grouped configuration store

## Scope

This specification owns the current behavior of `ao::rt::ConfigStore` and the common application configuration traits it invokes: lazy whole-file loading, named top-level groups, ordinary and exact decoding, in-memory replacement and removal, explicit flush, open modes, failure translation, and access serialization.

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
- The **document** is the complete in-memory YAML tree associated with that file.
- A **group** is a named child of the document's top-level mapping.
- A **seeded target** is the caller-provided object passed to a load operation, potentially already containing defaults or live values.
- An **ordinary decode** applies the permissive application configuration traits through `load()`.
- An **exact decode** applies `readExact` through `loadExact()`; exactness is recursive for reflected aggregates and vectors, not a universal schema system for every supported type.
- A **mutation** changes the in-memory document through `saveResult()`, `save()`, or `removeGroup()`.
- A **flush** emits and replaces the complete backing file from the current in-memory document.

## Invariants

- One non-copyable, non-movable `ConfigStore` instance owns one backing path, one YAML document, one parser input buffer, and one parser callback state.
- A successfully initialized document is cached for the remaining lifetime of the instance; the store does not merge or reload later external file changes.
- File inspection, file reading, or YAML parsing failure does not mark the document loaded, so a later group operation retries initialization.
- Group decode failure occurs after successful document initialization and does not cause a disk reload on the next operation.
- Every group in one instance shares the same whole-file document and flush boundary.
- Mutation and durability are distinct: a successful in-memory mutation does not update the backing file until a later successful `flush()`.
- The store has no dirty bit, revision, automatic flush, retry, rollback, semantic validation, observation, or durability acknowledgement.
- `ReadOnly` permits inspection and decoding but treats every mutation or flush attempt as an invariant fault.
- One instance has no internal synchronization; its owner confines all access to one executor or serializes it externally.
- Different instances targeting the same path do not coordinate and can replace one another's groups from stale whole-file documents.

## State model

The store has only a lazy-load state; it does not record whether the loaded document has been mutated.

| State | Document source | Next initialization attempt |
|---|---|---|
| Unloaded | Default-constructed in-memory tree. | `contains()`, `load()`, `loadExact()`, `saveResult()`, and `removeGroup()` inspect and, when present, parse the backing file. |
| Loaded | A parsed backing file or a new empty top-level mapping for a missing `ReadWrite` file. | No operation rereads the backing file. |

A failed inspection, read, or parse leaves the store Unloaded.
A missing backing file transitions a `ReadWrite` store to Loaded with an empty top-level mapping; the same condition returns `NotFound` and leaves a `ReadOnly` store Unloaded.

`flush()` is deliberately outside the lazy-load transition: it emits the current document without calling the initialization path.
Consequently, flushing a fresh instance writes its default-constructed document rather than first preserving an existing backing file.

## Commands and transitions

### Construction and open modes

Construction captures the backing path and initializes parser callback state with an owned copy of that path for diagnostics.
It performs no file access.

`ReadWrite` is the default mode and permits a missing file to become a new empty document.
`ReadOnly` requires the file to exist when initialization is attempted.
The mode is fixed for the lifetime of the store.

### Lazy initialization

The first initializing operation inspects the backing path.
For an existing file, it reads the complete byte sequence, parses it in place into the document, and retains the input buffer for the document lifetime.
Successful parsing accepts any YAML root kind; group lookup only addresses a top-level mapping.

An external change to the backing file after successful initialization is invisible to that instance.
A subsequent flush can therefore replace that change with the instance's cached whole-file document.

### Group inspection and loading

| Operation | Present readable group | Missing group or non-mapping root | Initialization or decode failure |
|---|---|---|---|
| `contains(group)` | Returns `true`. | Returns `false` without changing the document. | Returns the initialization error. |
| `load(group, target)` | Applies ordinary decode to the seeded target. | Succeeds without changing the target. | Returns the initialization or `FormatRejected` decode error. |
| `loadExact(group, target)` | Applies exact decode to the target. | Succeeds without changing the target. | Returns the initialization or `FormatRejected` decode error. |

Group absence is a normal outcome rather than `NotFound`.
The caller cannot distinguish absence from a successfully accepted group by inspecting the `Result` from either load operation; it uses `contains()` when that distinction is required.

### Ordinary decode

Ordinary decode is intended for seeded configuration values whose schema owner accepts permissive restore:

- Reflected aggregates require a mapping, visit known fields, preserve the seeded value for absent fields, and ignore unknown fields.
- An invalid present aggregate field makes the group decode fail, but fields visited earlier may already have changed the seeded target.
- Vectors and string-keyed maps require the matching container node, clear the destination, skip children that do not decode, and still accept the container.
- Fixed arrays visit available sequence children in order, ignore excess children, and retain existing elements when a child is invalid or absent.
- Invalid optional content clears the optional and makes its enclosing field decode fail.
- Arithmetic and boolean fields use the strict scalar conversions owned by the [reusable YAML adapter specification](yaml-adapter.md).
- Enums are stored as signed 32-bit numeric values and decoded by casting that value; the common trait does not validate membership in the enum's declared domain.
- Strong wrapper types supported through `raw()` use the codec for their underlying value.

Because ordinary decode can partially update its target before returning `FormatRejected`, it is not an object transaction.
A semantic owner that requires rollback prepares a separate candidate and installs it only after complete decode and semantic validation.

### Exact decode

Exact decode tightens reflected aggregates and vectors:

- An aggregate node must be a mapping with exactly the reflected field count, every reflected field must be present, and each field must recursively decode.
- Aggregate decoding uses a separate default-constructed candidate and assigns it to the caller's target only after the complete aggregate succeeds.
- A vector node must be a sequence, every element must recursively decode, and the target vector is replaced only after the complete sequence succeeds.

For scalars, optionals, arrays, maps, and other types without a specialized `readExact` overload, exact decode delegates to ordinary decode.
`loadExact()` therefore does not by itself provide field aliases, schema versions, enum-domain validation, semantic validation, or universal all-or-nothing behavior for every possible top-level type.
Versioned payload owners add those policies above the store.

### Group mutation

`saveResult(group, value)` initializes the document, converts a non-mapping root to a new top-level mapping, replaces the named group's encoded content in memory, and returns success.
Other cached groups are retained when the document was already a mapping.

`save(group, value)` calls `saveResult()` and discards its returned `Result`.
It is therefore a best-effort compatibility wrapper, not a failure-reporting save operation.
A caller that must preserve failure ordering calls `saveResult()` and requests `flush()` only after that mutation succeeds.

`removeGroup(group)` initializes the document and removes exactly the named top-level group.
It returns `true` when a group was removed and `false` for a missing group or non-mapping document.
Removal remains in memory until a successful flush and is idempotent after the first removal.

### Flush

`flush()` emits the complete current document and passes it to the [atomic file replacement contract](atomic-replacement.md) with owner-read/write permissions.
It returns the replacement helper's recoverable failure and does not clear, reload, roll back, or acknowledge any semantic dirty state.

`flush()` does not verify that initialization or a preceding mutation succeeded.
In particular:

- flushing an Unloaded store emits the default-constructed document;
- calling `save()` can hide an initialization failure; and
- flushing after that hidden failure can replace the backing file from the store's current document.

Payload owners that require failure-safe sequencing use the result-returning mutation path, flush only its successful document, and acknowledge their own snapshot only after flush succeeds.

## Failure and cancellation

| Condition | Observable channel |
|---|---|
| Backing-path inspection or file read fails | `Result` with `IoError`. |
| Backing file is missing in `ReadWrite` mode | Successful initialization of an empty document. |
| Backing file is missing in `ReadOnly` mode | `Result` with `NotFound`. |
| YAML syntax parsing throws | `Result` with `FormatRejected` and backing-file context. |
| A present group returns false from ordinary or exact decode | `Result` with `FormatRejected` and group context. |
| A standard exception escapes group decoding | `Result` with `FormatRejected` and group context. |
| Atomic file replacement fails | The helper's recoverable `Result` failure, currently `IoError`. |
| `save()`, `saveResult()`, `removeGroup()`, or `flush()` is called on a `ReadOnly` store | `ao::Exception` invariant fault. |

Encoding and YAML emission are not wrapped in a broad exception translation by `ConfigStore`.
An unexpected codec, allocation, or emitter exception is therefore not reclassified as a recoverable stored-data failure.

All store operations are synchronous and expose no cancellation point.
Scheduling, retry, coalescing, shutdown checkpoints, fallback, logging, notifications, and user presentation belong to the semantic owner and the surrounding workflow.

## Persistence and versioning

A successful flush replaces one complete YAML file; individual group mutations are not independently committed.
The [atomic file replacement specification](atomic-replacement.md) owns temporary-file creation, permission application, write barriers, replacement, cleanup, and platform-specific guarantees.

`ConfigStore` defines no file-level schema identifier, schema version, group registry, field alias, migration, or compatibility window.
Group names and payload schemas belong to their semantic owners and exact serialized references.
Reflected C++ field names and numeric enum values are current encodings, not an automatic promise that renamed members or reordered enum definitions remain compatible.

The exact default and platform paths for managed files belong to the [managed file locations reference](../../reference/persistence/location.md).

## Frontend observations

`ConfigStore` publishes no event, revision, progress, activity, or notification.
A runtime or frontend workflow decides whether a store failure is local validation, retained status, retryable background work, a notification, or a logged fallback according to its owning specification.

## Implementation map

- [`ConfigStore.h`](../../../app/include/ao/rt/ConfigStore.h) owns the public modes, group operations, templated encoding, and decode translation.
- [`ConfigStore.cpp`](../../../app/runtime/ConfigStore.cpp) owns lazy initialization, complete-file parsing, removal, emission, and atomic replacement invocation.
- [`ConfigTraits.h`](../../../app/include/ao/yaml/ConfigTraits.h) owns the common application codec and the ordinary/exact distinction.
- [`RymlAdapter.h`](../../../include/ao/yaml/RymlAdapter.h) owns reusable parser callbacks, file-reading helpers, scalar conversion, and tree node helpers.
- [`AtomicFile.h`](../../../include/ao/utility/AtomicFile.h), [`AtomicFile.cpp`](../../../lib/utility/AtomicFile.cpp), and [`AtomicFileWindows.cpp`](../../../lib/utility/AtomicFileWindows.cpp) own the lower file-replacement mechanism.

## Test map

- [`ConfigStoreTest.cpp`](../../../test/unit/runtime/ConfigStoreTest.cpp) protects round trips, seeded defaults, group absence, scalar boundaries, group overwrite and preservation, removal, read-only behavior, retry after initialization failure, and flush results.
- [`PlaybackSessionTest.cpp`](../../../test/unit/runtime/PlaybackSessionTest.cpp) protects the aggregate/vector exact-decode path and semantic validation above it.
- [`RymlAdapterTest.cpp`](../../../test/unit/utility/RymlAdapterTest.cpp) protects complete scalar parsing, range rejection, canonical booleans, recoverable file helpers, and callback diagnostic lifetime.
- [`AtomicFileTest.cpp`](../../../test/unit/utility/AtomicFileTest.cpp) protects replacement, owner-only permissions, and lower write-failure behavior.

The direct Unloaded-`flush()` path and the `save()`-discarded-initialization-failure sequence are observable in the implementation but do not yet have focused regression tests.

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
- [RFC 0014: observable atomic replacement](../../rfc/0014-observable-atomic-replacement.md)
- [RFC 0015: fail-closed grouped configuration transactions](../../rfc/0015-fail-closed-config-store.md)
