---
id: failure.error-value
type: reference
status: current
domain: system
summary: Enumerates the shared Aobus recoverable error value, code vocabulary, result wrapper, and diagnostic fields.
---
# Error value reference

## Scope and version

This reference enumerates the current in-process `ao::Error`, `ao::Result<T>`, and `makeError` surface from `include/ao/Error.h`.
These are C++ API types, not a serialized or wire format, and their enum ordinals are not compatibility identifiers.

Observable channel selection and translation rules belong to the [outcome channel specification](../../spec/failure/outcome-channel.md).

## Code boundary

The [system architecture](../../architecture/system-overview.md) places these types in the core-library layer, and the [failure and reporting architecture](../../architecture/failure-and-reporting.md) defines how higher layers consume them.
The public authority is `include/ao/Error.h`; runtime, UIModel, and frontends may depend on it, while the core type has no dependency on application reporting or presentation.

## Surface

### `ao::Error`

| Field | C++ type | Default | Meaning |
|---|---|---|---|
| `code` | `Error::Code` | `Generic` | Machine-readable recoverable category. |
| `message` | `std::string` | empty | Human-readable diagnostic context; never a machine-policy discriminator. |
| `location` | `std::source_location` | construction site | Cold-path diagnostic origin for logging; not user-visible recoverable semantics. |

### `ao::Error::Code`

`Code` is a scoped enum with underlying type `std::uint8_t`.

| Enumerator | Current meaning |
|---|---|
| `Generic` | Fallback when no more specific current code describes the recoverable failure. |
| `NotFound` | A requested target is absent at an API that also needs recoverable diagnostics. |
| `DeviceNotFound` | A requested or active media output device is unavailable. |
| `InvalidInput` | User- or caller-supplied command data is syntactically or semantically invalid. |
| `CorruptData` | External or persisted data violates a structural, size, or version invariant. |
| `FormatRejected` | Text, configuration, query, container, or schema input is malformed or rejected by the requested format contract. |
| `InitFailed` | A decoder, codec, backend, device, or other external facility could not initialize. |
| `IoError` | Filesystem, mapping, database, stream, or other IO failed. |
| `DecodeFailed` | Media decoding failed after the input or decoder had been accepted. |
| `SeekFailed` | A media seek operation failed. |
| `NotSupported` | The request is valid in shape, but the file type, codec, mode, capability, or operation is unsupported. |
| `InvalidState` | The runtime object state cannot currently satisfy the requested operation. |
| `Conflict` | A create or update conflicts with existing state or identity. |
| `ValueTooLarge` | External data is valid in shape but exceeds a serialized or configured size limit. |
| `ResourceExhausted` | A finite identifier, storage, or runtime resource pool is exhausted. |

Subsystem specifications narrow which codes a particular operation can produce.
This table does not make every code valid for every `Result` API.

### `ao::Result<T>`

`Result<T>` publicly derives from `std::expected<T, Error>` and inherits its forwarding constructors.
`Result<>` uses the default template argument `void` for commands without a success payload.
The type is marked `[[nodiscard]]`.

### `ao::makeError`

```cpp
std::unexpected<Error> makeError(
  Error::Code code,
  std::string message = {},
  std::source_location location = std::source_location::current());
```

The helper returns `std::unexpected<Error>`, moves the message into the value, and captures the caller location unless an explicit location is supplied.

## Validation rules

`Error::message` may be empty.
Callers preserve the code when adding contextual text and preserve the deepest useful `location` when propagating or translating a lower recoverable failure.

No API may use the numeric value of `Error::Code` as a persisted discriminator unless a separate versioned reference explicitly introduces such a format.
No recovery, severity, retry, or navigation policy may branch on message text.

## Compatibility and versioning

The code vocabulary is source-level API.
Adding, removing, or changing an enumerator requires updating this reference and every affected subsystem specification and test, but numeric enum stability is not promised for storage or IPC.

`location` is diagnostic-only and is not part of equality, persistence, user presentation, or a recoverable behavioral guarantee beyond retaining useful origin evidence through translations that already carry it.

## Examples

```cpp
return ao::makeError(ao::Error::Code::NotFound, "Track does not exist");
```

```cpp
ao::Result<> result{};
```

## Implementation authority

- [`Error.h`](../../../include/ao/Error.h) is the exhaustive type and enumerator authority.
- [`Exception.h`](../../../include/ao/Exception.h) defines the separate invariant-fault type and is intentionally outside this recoverable value surface.

## Test authority

- [`ErrorTest.cpp`](../../../test/unit/core/ErrorTest.cpp) protects construction, expected-value behavior, and source-location capture.
- Subsystem tests protect the code subsets promised by their own specifications.

## Related documents

- [Outcome channel specification](../../spec/failure/outcome-channel.md)
- [Failure and reporting architecture](../../architecture/failure-and-reporting.md)
