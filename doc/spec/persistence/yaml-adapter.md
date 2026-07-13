---
id: persistence.yaml-adapter
type: spec
status: current
domain: persistence
summary: Defines reusable RapidYAML callback containment, parsing, arena lifetime, file reading, node helpers, and scalar conversion.
---
# Reusable YAML adapter

## Scope

This specification owns the current behavior of the Core `ao::yaml` mechanisms in [`RymlAdapter.h`](../../../include/ao/yaml/RymlAdapter.h): RapidYAML callback construction, in-place and arena parsing, callback and source lifetime requirements, complete-file reading, borrowed node views, arena-owned key and value copies, and strict scalar conversion.

It does not own a YAML document schema, application configuration traits, aggregate or container decoding, reflected emission, group behavior, atomic file replacement, semantic validation, version migration, or a caller's public error translation.
Those contracts belong to the grouped store, model codec, payload specification, format reference, and semantic owner above this adapter.

The adapter is not a persistence service.
It provides reusable parsing and tree helpers to managed configuration, library interchange, layout documents, component state, CLI test support, and other code that owns its own document boundary.

## Code boundary

The [system architecture](../../architecture/system-overview.md) places reusable parsing mechanisms in Core libraries.
The [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md) places `ao::yaml` below application runtime, UIModel, frontend stores, paths, and application schemas.

The public implementation is the header-only [`include/ao/yaml/RymlAdapter.h`](../../../include/ao/yaml/RymlAdapter.h), exposed through the `ao_utility` public include and RapidYAML dependency configured by [`lib/utility/CMakeLists.txt`](../../../lib/utility/CMakeLists.txt).
It cannot depend on application runtime, UIModel, a frontend, logging policy, or an application payload type.

[`ConfigTraits.h`](../../../app/include/ao/yaml/ConfigTraits.h) is an application-runtime codec layered above the adapter.
[`Reflect.h`](../../../include/ao/yaml/Reflect.h) is a separate reflected output mechanism that uses the adapter's tree and arena helpers; its emitted field and enum vocabulary is not owned here.

## Terminology

- A **callback state** is an `ErrorCallbackState` containing the diagnostic filename used by RapidYAML error callbacks.
- A **source buffer** is the mutable `std::vector<char>` passed to in-place parsing.
- A **tree arena** is memory owned by a `ryml::Tree` for copied input, keys, and values.
- A **borrowed view** is a `std::string_view`, `ryml::csubstr`, or `ryml::substr` that does not own its referenced bytes.
- A **scalar conversion** parses one complete scalar into an arithmetic or string value without applying payload semantics.
- A **translation boundary** is a caller that catches adapter exceptions or interprets boolean conversion failure and maps them to its public outcome contract.

## Invariants

- `ErrorCallbackState` owns its filename string; a `ryml::Callbacks` value created from it stores only a raw pointer to that state.
- Callback state must outlive every tree operation that can invoke callbacks configured with that state.
- `parseInPlace()` mutates and borrows the supplied source buffer; the buffer must outlive the parsed tree's use of source-backed nodes.
- `parseInArena()` copies the supplied source into the tree arena; the input `string_view` need not outlive the call.
- Arena-copied keys and values remain valid only while the owning tree and the corresponding arena storage remain valid.
- Node key and scalar views are borrowed and never extend tree or source lifetime.
- Numeric conversion accepts only complete scalar consumption and a value representable by the destination type.
- A failed strict scalar conversion does not modify the caller's destination value.
- Boolean conversion accepts only the exact lower-case strings `true` and `false`.
- The adapter does not assign schema meaning, validate enum membership, select defaults, or decide whether a malformed value may be skipped.
- The adapter has no shared global callback state, parser singleton, lock, asynchronous work, or cancellation point.

## State model

The adapter itself retains no process-wide state.
Callers compose three lifetime domains:

| Domain | Owner | Required lifetime |
|---|---|---|
| Callback state | The caller or a containing store. | Until callbacks are replaced or every callback-using tree operation is complete. |
| In-place source buffer | The caller or a containing store. | Through every read of nodes that may reference the parsed source. |
| Tree arena | `ryml::Tree`. | Through every borrowed arena key, value, or arena-parsed node view. |

`ConfigStore` keeps its callback state, input buffer, and tree in one object so the first two lifetimes cover the cached document.
Short-lived importers and frontend adapters keep all three values within one parse operation.
A helper that returns a tree parsed with a local callback state must replace those callbacks or retain the state before the local state is destroyed.

## Commands and transitions

### Callback construction

`callbacks()` returns RapidYAML callbacks for basic, parse, and visit errors with no user data.
Those callbacks use `<buffer>` as diagnostic context.

`callbacks(state)` installs the same callback functions and stores a raw pointer to `state` as user data.
The callback functions throw `ao::Exception` with the callback category, owned filename, location values supplied by RapidYAML, and RapidYAML message.

The callbacks are an internal exception containment mechanism.
They do not return `Result` and do not independently classify a parser failure as `FormatRejected`.

### Parsing

`parseInPlace(tree, buffer, state)` performs this sequence:

1. Install callbacks referencing `state` on `tree`.
2. Pass the state's filename to RapidYAML as the source name.
3. Parse and mutate the complete `buffer` in place into `tree`.

`parseInArena(tree, source, state)` performs the same callback and source-name setup but asks RapidYAML to copy `source` into the tree arena before building nodes.

Both operations return `void` on success.
A RapidYAML diagnostic invokes the configured callback and throws `ao::Exception`; neither operation catches or translates that exception.
The tree's exact partial state after a thrown parser exception is not a usable document contract.

### File reading

`readFileResult(path)` opens the file in binary mode at end, obtains its size, seeks to the beginning, allocates one exact-size `std::vector<char>`, and reads that byte count.
It returns:

| Condition | Result |
|---|---|
| Open, size inspection, seek, or complete read fails | `IoError` with path context. |
| Complete read succeeds, including a zero-length file | The byte vector. |

The helper does not impose a document-size limit, text encoding, terminator, newline, or YAML validation.
Allocation, path-string conversion, and unrelated standard-library exceptions are not broadly converted to `IoError`.

`readFile(path)` is a throwing compatibility wrapper.
It returns the vector on success and otherwise throws `ao::Exception` using the recoverable error's message and diagnostic source location.

### Tree and node helpers

`toCsubstr()` and `toSubstr()` construct borrowed RapidYAML views without copying.
`copyToArena(tree, value)` and `copyToArena(node, value)` copy bytes into the owning tree arena.

`findChild(node, key)` returns RapidYAML's matching child reference or an unreadable reference when absent.
`setKey()` and `setValue()` first copy their input into the tree arena, so the caller's string storage may end after the call.

`scalarView()` and `keyView()` return an empty view when the node lacks the corresponding value or key.
An empty returned view alone therefore does not distinguish absence from a present empty scalar; callers use RapidYAML's `has_val()` or `has_key()` when that distinction matters.

### Scalar conversion

`tryParseScalar(text, value)` has these current modes:

- signed integral destinations parse through `std::int64_t`, require complete consumption, and check destination minimum and maximum;
- unsigned integral destinations parse through `std::uint64_t`, reject negative and other invalid text through `from_chars`, require complete consumption, and check the destination maximum;
- floating-point destinations parse directly into the destination type and require complete consumption;
- boolean destinations accept only exact `true` or `false` text.

Empty input is rejected for arithmetic destinations.
The parsed temporary is assigned to the caller only after all checks succeed.

`tryReadScalar(node, value)` first requires a scalar value and then applies the corresponding strict conversion.
The string-view overload borrows the node's storage; the string overload copies it.

`scalarAs<T>(node, context)` returns the converted scalar or `FormatRejected` with caller-supplied context.
It is the adapter's only result-returning scalar conversion helper.

`asBool(node, defaultValue)` and `asInt<T>(node, defaultValue)` are lenient accessors.
They return the supplied default for absence or malformed scalar text and provide no diagnostic that fallback occurred.
The model or payload owner decides where such coercion is permitted.

## Failure and cancellation

The low-level channel depends on the operation:

| Operation family | Failure channel |
|---|---|
| RapidYAML parsing or tree callback | `ao::Exception` thrown by the configured callback. |
| Complete-file read | `Result` with `IoError`. |
| `readFile()` compatibility wrapper | `ao::Exception`. |
| Strict `tryParseScalar` or `tryReadScalar` | `false`, with destination retained. |
| `scalarAs` | `Result` with `FormatRejected`. |
| Lenient `asBool` or `asInt` | Caller-supplied default. |

A public configuration, interchange, layout, or component-state boundary chooses the externally observable translation.
For example, `ConfigStore` catches parse exceptions and returns `FormatRejected`, while library import owns its own schema and rollback behavior.
Wrong node kinds, unsupported schema versions, and semantic rejection are not parser errors at this layer.

All operations are synchronous and expose no cooperative cancellation point.
The adapter does not log, notify, retry, or retain a failed document.

## Persistence and versioning

This mechanism has no serialized schema, format version, compatibility window, migration, canonical field order, or document ownership.
RapidYAML syntax acceptance is not an Aobus payload compatibility promise.

The [grouped configuration store specification](config-store.md) owns current application-trait decoding through this mechanism.
The [application managed-state surface](../../reference/persistence/application-config.md) owns managed group and version registration.
The [library YAML format](../../reference/library/format/yaml.md) owns library interchange shape.

## Frontend observations

The adapter publishes no state, event, activity, progress, or notification.
A frontend file adapter may log or fall back only according to the specification that owns that document and workflow.

## Implementation map

- [`RymlAdapter.h`](../../../include/ao/yaml/RymlAdapter.h) implements callback state, callbacks, parsing, file reading, tree helpers, scalar views, strict conversions, result conversion, and lenient accessors.
- [`lib/utility/CMakeLists.txt`](../../../lib/utility/CMakeLists.txt) exposes the Core include path and RapidYAML dependency through `ao_utility`.
- [`ConfigStore.cpp`](../../../app/runtime/ConfigStore.cpp), [`LibraryYamlImporter.cpp`](../../../app/runtime/library/LibraryYamlImporter.cpp), [`ShellLayoutComponentStateStore.cpp`](../../../app/linux-gtk/app/ShellLayoutComponentStateStore.cpp), and [`GtkLayoutPresets.cpp`](../../../app/linux-gtk/layout/document/GtkLayoutPresets.cpp) are representative parsing boundaries.
- [`ConfigTraits.h`](../../../app/include/ao/yaml/ConfigTraits.h) and [`Reflect.h`](../../../include/ao/yaml/Reflect.h) are higher codecs that reuse these primitives.

## Test map

- [`RymlAdapterTest.cpp`](../../../test/unit/utility/RymlAdapterTest.cpp) protects complete integral consumption, numeric range, unsigned-negative rejection, canonical booleans, missing-file `IoError`, scalar `FormatRejected`, and owned callback filename context.
- [`ConfigStoreTest.cpp`](../../../test/unit/runtime/ConfigStoreTest.cpp) protects translation of malformed YAML through a containing store and retry after initialization failure.
- Library transfer, layout model, and component-state tests protect their domain-specific use of the adapter without making those schemas part of this contract.

Current focused tests do not directly protect floating-point complete consumption, zero-length file reading, in-place buffer lifetime, arena-source independence, missing key/value view distinctions, arena copying after caller-string destruction, callback replacement before state destruction, or each individual file-read failure stage.

## Related documents

- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [System architecture](../../architecture/system-overview.md)
- [Grouped configuration store specification](config-store.md)
- [Atomic file replacement specification](atomic-replacement.md)
- [Outcome channel specification](../failure/outcome-channel.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
- [Library YAML format](../../reference/library/format/yaml.md)
- [Library YAML transfer specification](../library/runtime/yaml-transfer.md)
