---
id: persistence.yaml-adapter
type: spec
status: current
domain: persistence
summary: Defines reusable RapidYAML containment, lifetime, validation, explicit map and sequence traversal, scalar conversion, and arena-owning emission helpers.
---
# Reusable YAML adapter

## Scope

This specification owns the current behavior of the Core `ao::yaml` mechanisms in [`RymlAdapter.h`](../../../include/ao/yaml/RymlAdapter.h) and [`Serialization.h`](../../../include/ao/yaml/Serialization.h): RapidYAML callback construction, in-place and arena parsing, callback and source lifetime requirements, complete-file reading, borrowed node views, map and sequence validation, explicit map readers and writers, explicitly directed sequence traversal, required-child lookup, bounded field context, arena-owned key and value writes, and strict scalar conversion.

It does not own a YAML document schema, reflection-derived aggregate mapping, enum or identifier representation, reflected emission, group behavior, atomic file replacement, semantic validation, version migration, or a caller's public error translation.
Those contracts belong to the grouped store, model schema, payload specification, format reference, and semantic owner above this adapter.

The adapter is not a persistence service.
It provides reusable parsing and tree helpers to managed configuration, library interchange, layout documents, component state, CLI test support, and other code that owns its own document boundary.

## Code boundary

The [system architecture](../../architecture/system-overview.md) places reusable parsing mechanisms in Core libraries.
The [persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md) places `ao::yaml` below application runtime, UIModel, frontend stores, paths, and application schemas.

The public implementation is split between the header-only low-level [`include/ao/yaml/RymlAdapter.h`](../../../include/ao/yaml/RymlAdapter.h) and the explicit serialization helpers in [`include/ao/yaml/Serialization.h`](../../../include/ao/yaml/Serialization.h), exposed through the `ao_utility` public include and RapidYAML dependency configured by [`lib/utility/CMakeLists.txt`](../../../lib/utility/CMakeLists.txt).
It cannot depend on application runtime, UIModel, a frontend, logging policy, or an application payload type.

Managed-state schemas are owner-local application code layered above the adapter.
[`Reflect.h`](../../../include/ao/yaml/Reflect.h) is a separate one-way reflected output mechanism; `ConfigStore` never selects it, and it is not a managed-state reader or schema authority.

## Terminology

- A **callback state** is an `ErrorCallbackState` containing the diagnostic filename used by RapidYAML error callbacks.
- A **source buffer** is the mutable `std::vector<char>` passed to in-place parsing.
- A **tree arena** is memory owned by a `ryml::Tree` for copied input, keys, and values.
- A **borrowed view** is a `std::string_view`, `ryml::csubstr`, or `ryml::substr` that does not own its referenced bytes.
- A **scalar conversion** parses one complete scalar into an arithmetic or string value without applying payload semantics.
- A **field context** is a bounded diagnostic path assembled from owner-supplied map and field names.
- An **unknown-key policy** tells `validateMapKeys()` either to reject keys outside an owner-supplied allowlist or to permit dynamic keys while still rejecting duplicates.
- A **map writer** creates one mapping and writes only the literal scalar or child fields requested by its caller.
- A **map reader** validates one caller-supplied key policy and assigns only the required or optional scalar and child fields requested by its caller.
- A **sequence helper** traverses one explicitly selected sequence through a caller-supplied element writer or reader; it does not discover an aggregate schema.
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
- String conversion rejects YAML null and accepts a present non-null scalar, including an empty string.
- Map-key validation rejects entries without keys and duplicate keys under both unknown-key policies.
- `MapReader` retains its first structural, scalar, nested-value, or sequence failure and performs no later field assignments after that failure.
- A missing optional `MapReader` scalar or nested value preserves its destination; a malformed present value is a failure.
- A present YAML null is not a missing optional string; strict conversion returns `FormatRejected` and preserves the destination.
- `MapWriter` retains the first nested-value or sequence-writer failure and appends no later fields after that failure.
- Sequence deserialization requires a sequence and reports element failures with a bounded numeric index in the caller's context.
- Diagnostic context returned by the adapter is bounded to 160 bytes, including the `...` truncation suffix.
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

The helper does not impose a document-size limit, text serialization, terminator, newline, or YAML validation.
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

`requireMap(node, context)` and `requireSequence(node, context)` return `FormatRejected` unless the node has the requested kind.
They do not reinterpret null, a scalar, or the other container kind.

`validateMapKeys(node, allowedKeys, context, policy)` first requires a mapping, then rejects keyless and duplicate entries.
With `UnknownKeyPolicy::Reject`, it also rejects every key outside `allowedKeys`.
With `UnknownKeyPolicy::Allow`, dynamic keys are accepted but duplicate detection remains active.
The helper validates structural membership only; it does not require allowlisted fields to be present.

`requireChild(node, key, context)` returns the readable child or `FormatRejected` when absent.
`requireScalar<T>(node, key, context)` composes required-child lookup with strict scalar conversion.

`appendChild(node, key)` creates a child and copies the key into the owning tree arena.
`writeScalar()` writes strings, booleans, and arithmetic values.
String bytes are copied into the arena and marked double-quoted so values such as `null`, `true`, and `42` remain strings after emission and parsing; booleans use canonical lower-case text, and numeric formatting is delegated to RapidYAML/c4.

`MapWriter(node)` establishes `node` as a mapping.
`scalar(key, value)` appends one arena-owned scalar field; `value()`, `sequence()`, and `scalarSequence()` append an explicitly named nested value through the caller-selected writer.
The map writer retains the first nested-writer failure, skips later fields, and returns that result from `finish()`.
The writer does not enumerate members, infer field names, or decide whether a field should be omitted.

`MapReader(node, allowedKeys, context, policy)` validates the mapping and key policy at construction.
`requiredScalar()`, `requiredValue()`, `requiredSequence()`, and `requiredScalarSequence()` require and assign explicitly named fields.
The corresponding optional operations assign only a present valid value and otherwise preserve the supplied destination.
The reader retains the first failure and skips later assignments.
`finish(value)` returns that failure or the supplied complete value, while `result()` lets a schema validate or deserialize nested children before completing its candidate.

`writeSequence(node, values, elementWriter)` establishes a sequence and invokes the caller's writer once per element in order, stopping on the first returned error.
`readSequence<T>(node, context, elementReader)` first requires a sequence, reserves its result, and invokes the reader with `context.<index>` for each child in order.
`writeScalarSequence()` and `readScalarSequence<T>()` are the corresponding strict-scalar specializations.

`writeStringMap()` and `readStringMap<Map>()` traverse an explicitly selected string-keyed mapping through a caller-supplied value writer or reader.
They reject empty keys, copy emitted keys into the tree arena, and apply duplicate-key validation on deserialize; they do not infer a map field or payload schema.
An empty key supplied by the serialize range is invalid live state and returns `InvalidState`; an empty key in deserialized YAML is malformed persisted input and returns `FormatRejected`.

`boundedErrorContext()` truncates arbitrary context to `kMaximumErrorContextBytes`, currently 160 bytes.
`fieldContext()` appends a dot-separated field and applies the same bound.

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
Both string overloads reject YAML null rather than converting it to empty text.

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
| Node-kind, key, child, `MapReader`, sequence, or `scalarAs` validation | `Result` with `FormatRejected` and bounded caller context. |
| Caller-supplied sequence or string-map writer or reader | The first returned error, without adapter-side logging or translation. |
| Empty string-map key | `InvalidState` while serialization live state; `FormatRejected` while deserialization persisted input. |
| Lenient `asBool` or `asInt` | Caller-supplied default. |

A public configuration, interchange, layout, or component-state boundary chooses the externally observable translation.
For example, `ConfigStore` catches parse exceptions and returns `FormatRejected`, while library import owns its own schema and rollback behavior.
Wrong node kinds, unsupported schema versions, and semantic rejection are not parser errors at this layer.

All operations are synchronous and expose no cooperative cancellation point.
The adapter does not log, notify, retry, or retain a failed document.

## Persistence and versioning

This mechanism has no serialized schema, format version, compatibility window, migration, canonical field order, or document ownership.
RapidYAML syntax acceptance is not an Aobus payload compatibility promise.

The [grouped configuration store specification](config-store.md) owns explicit owner-schema invocation through this mechanism.
The [application managed-state surface](../../reference/persistence/application-config.md) owns managed group and version registration.
The [library YAML format](../../reference/library/format/yaml.md) owns library interchange shape.

## Frontend observations

The adapter publishes no state, event, activity, progress, or notification.
A frontend file adapter may log or fall back only according to the specification that owns that document and workflow.

## Implementation map

- [`RymlAdapter.h`](../../../include/ao/yaml/RymlAdapter.h) implements callback state, parsing, file reading, arena and borrowed-view helpers, strict scalar reads, and lenient accessors.
- [`Serialization.h`](../../../include/ao/yaml/Serialization.h) implements node and key validation, explicit map/sequence composition, bounded field context, required fields, and arena-owning scalar writes.
- [`lib/utility/CMakeLists.txt`](../../../lib/utility/CMakeLists.txt) exposes the Core include path and RapidYAML dependency through `ao_utility`.
- [`ConfigStore.cpp`](../../../app/runtime/ConfigStore.cpp), [`LibraryYamlImporter.cpp`](../../../app/runtime/library/LibraryYamlImporter.cpp), [`ShellLayoutComponentStateStore.cpp`](../../../app/linux-gtk/app/ShellLayoutComponentStateStore.cpp), and [`GtkLayoutPresets.cpp`](../../../app/linux-gtk/layout/document/GtkLayoutPresets.cpp) are representative parsing boundaries.
- Owner-local runtime, UIModel, and frontend schemas reuse these primitives; [`Reflect.h`](../../../include/ao/yaml/Reflect.h) remains a separate one-way output helper.

## Test map

- [`RymlAdapterTest.cpp`](../../../test/unit/utility/RymlAdapterTest.cpp) protects complete scalar consumption, numeric range, unsigned-negative rejection, canonical booleans, null-string rejection, bounded context, missing-file `IoError`, and callback filename ownership.
- [`YamlSerializationTest.cpp`](../../../test/unit/utility/YamlSerializationTest.cpp) protects quoted string type preservation, node kinds, required children, duplicate and unknown keys, map-reader assignment, explicit-null rejection, failure order, map-writer failure order and arena ownership, sequence index context, bounded field context, and dynamic string-map boundary classification.
- [`ConfigStoreTest.cpp`](../../../test/unit/runtime/ConfigStoreTest.cpp) protects translation of malformed YAML through a containing store and retry after initialization failure.
- Library transfer, layout model, and component-state tests protect their domain-specific use of the adapter without making those schemas part of this contract.

Current focused tests do not directly protect zero-length file reading, in-place buffer lifetime, callback replacement before state destruction, or each individual file-read failure stage.

## Related documents

- [Persistence and managed-state architecture](../../architecture/persistence-and-managed-state.md)
- [System architecture](../../architecture/system-overview.md)
- [Grouped configuration store specification](config-store.md)
- [Atomic file replacement specification](atomic-replacement.md)
- [Outcome channel specification](../failure/outcome-channel.md)
- [Application managed-state surface](../../reference/persistence/application-config.md)
- [Managed-state schema development guide](../../development/managed-state-schemas.md)
- [Library YAML format](../../reference/library/format/yaml.md)
- [Library YAML transfer specification](../library/runtime/yaml-transfer.md)
