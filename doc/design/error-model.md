# Error Model

The core library uses two failure mechanisms with a strict semantic boundary:

- **External failure returns `ao::Result<T>`**. This includes user input,
  file contents, metadata formats, IO, devices, library database contents,
  unsupported versions, and resource limits reached while handling external
  data. These failures are expected outcomes that callers can report, skip,
  retry, or recover from.
- **Internal invariant failure may throw `ao::Exception`**. This is reserved for
  programmer errors and impossible states: object lifetime misuse, invalid API
  preconditions, out-of-range accessors with `at()` semantics, and corrupted
  in-memory invariants that the caller could not reasonably handle.

The short rule is:

```text
External failure => Result
Internal bug or invariant violation => throw
```

Being off the real-time path is not a reason to throw. It only affects
performance sensitivity; it does not change whether a failure is part of the
domain.

## Result

`ao::Result<T>` is `std::expected<T, ao::Error>` (`include/ao/Error.h`). Use it
for recoverable failures and all failures caused by data or state outside the
current function's control.

Preferred error-code meanings:

| Code | Meaning |
| --- | --- |
| `InvalidInput` | User-supplied text or command data is syntactically or semantically invalid. |
| `CorruptData` | A persisted record or file claims an impossible structure, size, or version relationship. |
| `FormatRejected` | A media/query/config format is unsupported or malformed for the requested operation. |
| `IoError` | Filesystem, mmap, LMDB, or other storage IO failed. |
| `NotFound` | A requested lookup target is absent, and the API already needs `Result` for other diagnostics. |
| `NotSupported` | The request is valid but the feature, device mode, codec mode, or file type is not supported. |
| `Conflict` | A create/update failed because the target state already exists or otherwise conflicts. |
| `ValueTooLarge` | External data is valid in shape but exceeds a serialized or configured size limit. |
| `InvalidState` | The object is in a runtime state that cannot satisfy the requested operation. |
| `ResourceExhausted` | A finite storage, ID, or runtime resource pool is exhausted while handling external data. |
| `Generic` | Temporary fallback when no existing code fits; do not add new uses without first considering a better code. |

APIs that already model absence precisely should keep using `std::optional<T>`
for pure in-memory lookup misses or parse helpers with a single failure mode.
Examples include a dictionary lookup miss and Base64 malformed input. Once the
same API needs an error category or diagnostic, use `Result<T>` and represent a
lookup miss as `Error::Code::NotFound` instead of nesting `std::optional<T>`.

### Return Value Shape

Choose the return shape that models the successful domain value before adding
an error channel:

| Shape | Use when |
| --- | --- |
| `bool` | The API is a pure binary predicate and has no external failure mode. |
| `T*` / `T const*` | The API is an in-memory lookup that returns a borrowed object; `nullptr` means absent. |
| `std::optional<T>` | The API returns a value object and absence is the only normal miss state. |
| `Result<>` | The API is a command that can fail but has no successful domain value. |
| `Result<T>` | The API returns a value and can fail for external reasons; lookup misses use `NotFound`. |
| `Result<Enum>` | The API has multiple normal success states and can also fail externally. |

Avoid `Result<bool>` for new APIs. If the `bool` means command success, use
`Result<>`. If it means a normal domain state, use `bool` for a pure predicate
or a named enum when the operation can also fail. Also avoid
`Result<std::optional<T>>`; it usually mixes "absence is normal" with "absence
is diagnostic" in one contract. Use `std::optional<T>` for pure local lookup or
`Result<T>` with `NotFound` when the operation needs diagnostics.

## Exceptions

Throw only when continuing would hide a programming error or broken invariant.
Acceptable examples:

- using a transaction-bound writer after its transaction was committed;
- dereferencing an invalid iterator or calling an `at()`-style accessor out of
  range;
- hitting an unexpected LMDB cursor error while advancing an iterator over an
  already-open transaction; cursor EOF is normal and must not throw;
- reaching an impossible enum value in an internal switch;
- assertion-like layout misuse where the caller passed a span that violates an
  API precondition already documented as internal.

Do not throw for malformed files, unsupported extensions, parse failures,
recoverable database-content corruption, missing on-disk records, IO errors, or
values that are too large because they came from external metadata. Return
`Result` and let the caller decide whether to skip, report, retry, or abort the
operation.

Third-party libraries may still throw at their own boundaries. Catch those
exceptions at the narrow wrapper boundary and translate them to `Result` unless
the exception represents a true invariant failure in our code.

A subsystem may also use a private exception as short-range internal control
flow, to collapse repeated `if (!result) return std::unexpected{...}` plumbing,
provided it is translated back to `Result` at the subsystem boundary and never
escapes. The convention is per-subsystem, not a shared core type: define one
domain-named leaf exception that derives `ao::Exception` and carries the `Error`
(for example `ao::audio::detail::DecoderException`), keep it in that subsystem's
`detail/` header and `detail` namespace so it is not part of the public API,
raise it through a `throw<Domain>Error` helper, and catch exactly that leaf at a
`noexcept` `Result` boundary. Keep the catch narrow to the leaf type: a
non-domain fault such as `std::bad_alloc` then fail-fast terminates instead of
being laundered into a recoverable code. Do not promote this exception into the
public `ao::Exception` vocabulary, do not share a common `Error`-carrying base
across subsystems (a base invites a too-wide `catch` that re-enables
cross-subsystem laundering), and do not name it for being "internal" — it carries
a recoverable `Error`, the opposite of the invariant-failure throws above.

## Current Migration

The subsystem status below records the intended boundary for areas that recently
changed or still have local containment rules:

| Area | Status |
| --- | --- |
| `ao/tag` | Public entry points return `Result`: `TagFile::open` reports mapping errors as `IoError`, unsupported extensions as `NotSupported`, and successful opens as non-null owners; `TagFile::loadTrack` reports malformed metadata as `CorruptData`. Per-format `loadTrackImpl()` parsers return `Result<TrackBuilder>` and translate contained corruption-detection exceptions at the format boundary. Within parser implementations, the `id3v2::FrameView` ctor + `layout<>()` raise recoverable failures through `detail::throwTagError` (defined in `include/ao/tag/detail/TagError.h`, `ao::tag::detail`, not public) — errors carry an explicit `CorruptData` code and preserve the throw-site `source_location`. Each boundary first catches `detail::TagException const&` and returns `std::unexpected{ex.error()}` (preserving code + location), then catches `ao::Exception const&` as a fallback for corruption throws from the shared `ao::media` parsing layer (which uses the public `Exception` base). `mpeg::FrameView` table accesses in `Frame.cpp` use `operator[]` (the table dimensions match the header field encoding exactly; `isValid()` is the front gate used by `locate()`). MP4 audio-property extraction is best-effort: optional malformed/non-version-0 `mdhd` atoms are skipped before any layout precondition is evaluated, while required metadata parsing still reports corrupt input through the normal boundary. The previous `catch (std::out_of_range const&)` at each boundary is removed — it was dead (the single `throw std::out_of_range` in `utility::bytes::requireLayout` has zero callers, and the `std::array::at()` calls in `Frame.cpp` could never overflow given the field-encoding ranges). Single-guard outer shells (`TagFile::loadTrack`/`open`/`mappedResult`), the flac magic check, and the `gsl_*` asserts in `utility/ByteView.h` stay as-is. The `TagFile` base does not catch parser exceptions as a fallback. |
| `ao/media` byte views | Public decode/tag entry points return `Result`; `mp4::Demuxer::parseTrack` translates contained `detail::MediaException` failures back to `Result` at its boundary. MP4 sample-table/timing private helpers throw `FormatRejected` through `detail::throwMediaError` instead of returning `bool`, preserving specific diagnostics for malformed container data. FLAC metadata block iteration checks the current block size before honoring the last-block marker, so truncated final metadata blocks report `CorruptData`. Low-level helpers should either return `Result` directly or remain contained behind a translating boundary during migration. |
| `ao/lmdb` | Environment open, low-level transaction begin/commit, database open, and the fallible `create`/`update`/`clear`/`append` write operations return `Result`: duplicate create is `Conflict`, exhausted integer append key space is `ResourceExhausted`, and other LMDB operation failures are `IoError`. Point reads collapse to their narrowest honest shape because absence is their only recoverable outcome: `Reader`/`Writer` `get` return `std::optional<std::span<...>>` (empty means absent), `maxKey` returns the largest key or `0`, and `Writer::del` returns `bool` (`true` if a row was removed). These never surface a recoverable error code: a non-`MDB_NOTFOUND` lookup/delete failure throws, because the on-disk corruption that would produce `MDB_CORRUPTED` here equally surfaces as `SIGBUS` through the mmap, which no `Result` can intercept — a recoverable channel at this layer would be a false promise. Setup/write wrappers propagate inner `Error` objects directly so diagnostic `source_location` is preserved. Use the factory functions for fallible LMDB setup; public constructor bridges are not kept. Cursor-backed iteration keeps normal iterator semantics: EOF is normal, while unexpected cursor-open/traversal failures throw for the same reason. Writer lifetime misuse after transaction commit still throws. |
| `ao/library` persisted views/stores | Store read entry points pick their shape by their actual failure surface, not by family uniformity. Lookups whose only recoverable outcome is absence return `std::optional<T>` (`ListStore`/`TrackStore`/`ResourceStore` `get`); lookups that also validate the persisted record and can report `CorruptData`/`ValueTooLarge`/version mismatch keep `Result<T>` (`MetaStore::load`, `FileManifestStore::Reader::get`). Missing metadata headers and lookup misses are `NotFound`, corrupt persisted records and unsupported library versions are `CorruptData`, oversized manifest URIs are `ValueTooLarge`, exhausted resource ID space is `ResourceExhausted`, and `MusicLibrary::open` reports open/setup failures without throwing. Store commands such as track/list/resource create, update, and clear return `Result<>` or `Result<T>` rather than using `.value()` to rethrow `std::bad_expected_access`; `DictionaryStore::put()` follows the same rule because it writes through LMDB even though the dictionary also owns an in-memory lookup index. Deletes return `bool` (`TrackStore::Writer::remove`, `ListStore::Writer::del`) — `true` if a row was removed — because corruption throws at the LMDB layer and absence is not an error; idempotent manifest removal returns `Result<>` only because URI validation can reject the key first. Once a library session is open, `MusicLibrary::readTransaction()` and `writeTransaction()` treat transaction begin failure as a severe runtime failure and throw through the high-level facade; callers that need recoverable handling can use the lower-level LMDB factories directly. `TrackBuilder` serialization preparation (`PreparedHot::create`/`PreparedCold::create`) raises recoverable failures through `detail::throwLibraryError` and translates them to `Result` at each public prepare/serialize boundary (`prepareHot`, `prepareCold`, `prepare`, `serializeHot`, `serializeCold`, and `serialize`) via a narrow `catch (detail::LibraryException const&)`; the leaf lives in `include/ao/library/detail/LibraryError.h` (`ao::library::detail`) and is not public; overflow validations carry `ValueTooLarge` explicitly while propagated dict/resource failures keep their own code and location. Shallow LMDB/store propagation keeps direct `Result` flow and propagates inner `Error` objects without recreating locations. Single-guard store sites and invariant `throwException`/`gsl_*` checks stay as-is. Direct view constructors and at-style accessors may still throw for internal precondition misuse. |
| `ao/rt` library read models | Runtime read-model helpers may expose `std::optional<T>` or empty snapshots for domain absence, but only after translating `Error::Code::NotFound`. Other storage errors from store `Result<T>` values are either propagated through a `Result<>` operation such as YAML export, or thrown as high-level `ao::Exception` from UI/runtime convenience APIs that cannot report recoverable failures. Do not test a store `Result<T>` as a plain boolean in runtime code unless the non-`NotFound` branch is handled explicitly. |
| `ao/query` | Parser, query compiler, format compiler, and field-resolution entry points return `Result` for syntax/semantic rejection and use `FormatRejected` for user-facing diagnostics. Completion-only field probing uses `std::optional<Field>` because absence is the only needed state. Within `QueryCompiler` and `FormatCompiler`, private compile helpers (compileExpression, compilePredicate, compileBinary, compileUnary, compileExists, compileVariable, compileConstant, compileList, compileRange, compileIn, compileInWithList, compileInRange, compileInSetList, appendInSetValue, and the anonymous-namespace helpers toOpCode/unitMultiplier/scaleUnitSegment/scaleUnitConstant) raise recoverable failures through `detail::throwQueryError` (defined in `include/ao/query/detail/QueryError.h`, `ao::query::detail`, not public) — errors carry an explicit `FormatRejected` code and preserve the throw-site `source_location`. A single `try`/`catch (detail::QueryException const&)` at `QueryCompiler::compile()` and `FormatCompiler::compile()` translates them back to `Result`, keeping the public `compileQuery`/`compileFormat` entry points non-throwing. The catch is deliberately narrow (only `QueryException`) so non-domain faults are not laundered into a recoverable code. `OperatorTable::operatorInfo()` uses `operator[]` with a `gsl_Expects` guard instead of `.at()`, making an out-of-range operator an invariant failure (terminate) rather than a recoverable `std::out_of_range`. |
| `ao/audio` | Decoder, source, and backend entry points return `Result` for every external failure (`IDecoderSession::open`/`seek`/`readNextBlock`, `ISource::seek`, `IBackend::open`/`setProperty`/`property`); the layer never throws on external input and never collapses a fault to a codeless `Generic`. The `IDecoderSession` and `ISource::seek` boundary methods are `noexcept`: the contract that this layer never throws to its caller is enforced at compile time (an override that drops `noexcept` is ill-formed) and fail-fast at runtime (an escaping exception such as `std::bad_alloc` terminates instead of being silently reinterpreted as a decode error). Within decoder session implementations (such as `Mp3DecoderSession`), each public method keeps a single internal style: failures are raised with `detail::throwDecoderError` — the helper and the `detail::DecoderException` it throws are an implementation detail, defined in `ao/audio/detail/DecoderError.h` (the `ao::audio::detail` namespace), not part of the public decoder API — and one `try`/`catch (detail::DecoderException const&)` translates them to `Result` at the boundary — a body that calls no throwing helper (such as `AlacDecoderSession::readNextBlock`) stays purely `Result`-returning rather than wrapping a no-op `try`. Errors propagated from an inner `Result` keep their original diagnostic location via `detail::throwDecoderError(Error)`; freshly originated failures capture the call site via `detail::throwDecoderError(code, message)`. The catch is deliberately narrow (only `DecoderException`) so non-domain faults are not laundered into a recoverable code. End of stream is modeled as a value, not an error: `readNextBlock` returns `Result<PcmBlock>` whose `endOfStream` flag marks the final block, and `StreamingSource::decodeNextBlock` returns a `DecodeBlockStatus` enum for its normal success states, signalling decode failure by throwing `DecoderException` to the source's translating boundary. This subsystem uses the media-domain codes from `Error.h`: `InitFailed` for decoder/codec/device initialization, `DecodeFailed` for decode-time payload failures, `SeekFailed` for seek failures, `DeviceNotFound` for an absent device, alongside `NotSupported`/`FormatRejected`/`IoError` for unsupported formats and container IO. `createDecoderSession` returns `Result<std::unique_ptr<IDecoderSession>>` — `NotSupported` for an unsupported extension or unrecognized container codec, `IoError` for an unreadable container — instead of a `nullptr` that conflated "cannot play" with "cannot open". `TrackSession` threads the concrete `Error` through format negotiation and PCM-source creation (`NotSupported` when the device cannot accept the stream format) rather than flattening it to a message-only `bool` out-parameter. `IBackendProvider::shutdown()` is `noexcept` by contract. |

New code should follow the target rule even when working near legacy code.
