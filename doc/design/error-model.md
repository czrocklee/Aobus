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
| `ao/tag` | Public entry points return `Result`: `TagFile::open` reports mapping errors as `IoError`, unsupported extensions as `NotSupported`, and successful opens as non-null owners; `TagFile::loadTrack` reports malformed metadata as `CorruptData`. Per-format `loadTrackImpl()` parsers return `Result<TrackBuilder>` and translate contained corruption-detection exceptions at the format boundary. Parsers raise `CorruptData` through `detail::throwTagError`, and the shared `ao::media` parsing layer through `throwMediaError`; each format boundary catches those leaves and converts them to `Result` — flac catches both `detail::TagException` and `media::detail::MediaException`, while mp4/mpeg catch only `TagException` (their paths raise nothing else). `mpeg::FrameView` table accesses use `operator[]` because the field-encoding ranges match the table dimensions. MP4 audio-property extraction is best-effort: malformed or non-version-0 `mdhd` atoms are skipped, while required metadata still reports corruption through the normal boundary. |
| `ao/media` byte views | Public decode/tag entry points return `Result`; `mp4::Demuxer::parseTrack` catches `detail::MediaException` and translates it to `Result`. MP4 sample-table/timing helpers raise `FormatRejected` via `throwMediaError` (rather than returning `bool`) to keep specific diagnostics for malformed container data. FLAC block iteration validates the current block size before honoring the last-block marker, so a truncated final block reports `CorruptData`. |
| `ao/lmdb` | Fallible setup/write factories return `Result`: duplicate create is `Conflict`, exhausted integer append key space is `ResourceExhausted`, other failures are `IoError`. Point reads collapse to their narrowest shape because absence is their only recoverable outcome: `get` returns `std::optional<std::span<...>>` (empty means absent), `maxKey` returns the largest key or `0`, and `Writer::del` returns `bool`. They never surface a recoverable code — a non-`MDB_NOTFOUND` failure throws, because the corruption that would produce `MDB_CORRUPTED` equally surfaces as `SIGBUS` through the mmap, which no `Result` can intercept. Use the factory functions (public constructor bridges are not kept). Cursor EOF is normal; unexpected cursor failures throw, as does writer use after its transaction commits. |
| `ao/library` persisted views/stores | Read shapes follow the actual failure surface: absence-only lookups return `std::optional<T>` (`ListStore`/`TrackStore`/`ResourceStore::get`); lookups that also validate the persisted record return `Result<T>` (`MetaStore::load`, `FileManifestStore::Reader::get`). Misses are `NotFound`, corrupt or unsupported-version records are `CorruptData`, oversized manifest URIs are `ValueTooLarge`, exhausted resource IDs are `ResourceExhausted`; `MusicLibrary::open` reports setup failures without throwing. Create/update/clear commands return `Result<>`/`Result<T>` (never `.value()`); deletes return `bool` because corruption throws at the LMDB layer and absence is not an error, except idempotent manifest removal (`Result<>`, since URI validation can reject the key first). Once a session is open, `read/writeTransaction()` treat begin failure as severe and throw. `TrackBuilder` serialization raises its `ValueTooLarge` overflow checks via `detail::throwLibraryError`, translated to `Result` at the `prepare*`/`serialize*` boundaries. View constructors and at-style accessors may still throw on internal precondition misuse. `LibraryScanner::buildPlan` returns `Result<ScanPlan>`: per-file problems stay in-band as `ScanClassification::Error` items so the rest of the plan still applies, but a failure that prevents any plan at all - a missing music root (`NotFound`) or a filesystem walk that cannot start (`IoError`) - fails the whole call, so the caller reports it instead of mistaking an unscannable root for an empty, up-to-date library. |
| `ao/rt` library read models | Runtime read-model helpers may expose `std::optional<T>` or empty snapshots for domain absence, but only after translating `Error::Code::NotFound`. Other storage errors from store `Result<T>` values are either propagated through a `Result<>` operation such as YAML export, or thrown as high-level `ao::Exception` from UI/runtime convenience APIs that cannot report recoverable failures. Do not test a store `Result<T>` as a plain boolean in runtime code unless the non-`NotFound` branch is handled explicitly. |
| `ao/query` | Parser, query/format compiler, and field-resolution entry points return `Result`, using `FormatRejected` for user-facing diagnostics. Completion-only field probing uses `std::optional<Field>` because absence is the only needed state. The private compile helpers raise `FormatRejected` via `detail::throwQueryError`, translated back to `Result` by a single catch at `QueryCompiler::compile()`/`FormatCompiler::compile()`. `OperatorTable::operatorInfo()` uses `operator[]` with a `gsl_Expects` guard instead of `.at()`, making an out-of-range operator an invariant failure rather than a recoverable `std::out_of_range`. |
| `ao/audio` | Decoder, source, and backend entry points return `Result` for every external failure (`IDecoderSession::open`/`seek`/`readNextBlock`, `ISource::seek`, `IBackend::open`/`setProperty`/`property`); the layer never throws on external input and never collapses a fault to a codeless `Generic`. The `IDecoderSession` and `ISource::seek` boundary methods are `noexcept`: the contract that this layer never throws to its caller is enforced at compile time (an override that drops `noexcept` is ill-formed) and fail-fast at runtime (an escaping exception such as `std::bad_alloc` terminates instead of being silently reinterpreted as a decode error). Within decoder session implementations (such as `Mp3DecoderSession`), each public method keeps a single internal style: failures are raised with `detail::throwDecoderError` — the helper and the `detail::DecoderException` it throws are an implementation detail, defined in `ao/audio/detail/DecoderError.h` (the `ao::audio::detail` namespace), not part of the public decoder API — and one `try`/`catch (detail::DecoderException const&)` translates them to `Result` at the boundary — a body that calls no throwing helper (such as `AlacDecoderSession::readNextBlock`) stays purely `Result`-returning rather than wrapping a no-op `try`. Errors propagated from an inner `Result` keep their original diagnostic location via `detail::throwDecoderError(Error)`; freshly originated failures capture the call site via `detail::throwDecoderError(code, message)`. The catch is deliberately narrow (only `DecoderException`) so non-domain faults are not laundered into a recoverable code. End of stream is modeled as a value, not an error: `readNextBlock` returns `Result<PcmBlock>` whose `endOfStream` flag marks the final block, and `StreamingSource::decodeNextBlock` returns a `DecodeBlockStatus` enum for its normal success states, signalling decode failure by throwing `DecoderException` to the source's translating boundary. This subsystem uses the media-domain codes from `Error.h`: `InitFailed` for decoder/codec/device initialization, `DecodeFailed` for decode-time payload failures, `SeekFailed` for seek failures, `DeviceNotFound` for an absent device, alongside `NotSupported`/`FormatRejected`/`IoError` for unsupported formats and container IO. `createDecoderSession` returns `Result<std::unique_ptr<IDecoderSession>>` — `NotSupported` for an unsupported extension or unrecognized container codec, `IoError` for an unreadable container — instead of a `nullptr` that conflated "cannot play" with "cannot open". `TrackSession` threads the concrete `Error` through format negotiation and PCM-source creation (`NotSupported` when the device cannot accept the stream format) rather than flattening it to a message-only `bool` out-parameter. `IBackendProvider::shutdown()` is `noexcept` by contract. |

New code should follow the target rule even when working near legacy code.
