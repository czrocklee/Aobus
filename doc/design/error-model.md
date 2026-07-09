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

## Persisted Binary Layouts

The LMDB-backed binary layouts (`TrackView`, `ListView`, `FileManifestView`
over `TrackLayout`/`ListLayout`/`FileManifestLayout`) have their own read-side
containment rules, tuned to what LMDB actually guarantees and to the fact that
these records are app-private: the only producers are our builders
(`TrackBuilder`, `ListBuilder`, `FileManifestBuilder`), unlike `ao/tag` and
`ao/media` inputs, which stay fully defensive because they parse foreign
files.

**The write side is the single validation boundary.** Builders enforce every
layout invariant when serializing, and the store write paths gate record size
and alignment. The zero-copy path preserves this: `prepare*()` returns an
immutable snapshot that owns every byte it will write with all header fields
overflow-checked and frozen, so mutating or destroying the builder between
prepare and write cannot produce a record the boundary never validated. The `size % 4 == 0` write gate is load-bearing, not cosmetic:
together with the 4-byte integer keys and LMDB's node layout it keeps every
value in these databases 4-byte aligned, which is what lets read paths map
records with typed views at all.

**LMDB's guarantees bound what read-side checking can buy.** A committed
transaction returns byte-identical data (copy-on-write pages, snapshot
isolation), and the default read-only mmap means in-process stray writes
cannot corrupt the database. But LMDB does not checksum pages - external file
corruption arrives silently, and a truncated file surfaces as `SIGBUS` that no
validation intercepts. Deep read-side validation therefore cannot deliver
crash-proof reads; it only converts one rare failure shape into another at a
per-row cost.

**The read side runs one O(1) structural gate per record.** The gate
establishes memory safety only: the fixed header fits and is aligned, and
every derived slice (title/tag extents, URI range, extension block slots)
stays inside the record span. It runs once per view - eagerly for hot data,
lazily cached for cold data - and accessors trust the gated slices afterward.
Ranges that are data-driven per element (custom metadata value offsets) get a
per-access clamp instead of a pre-scan. Accessors never throw and never read
out of bounds.

**Corruption is record-granular and non-silent.** A record that fails its
gate poisons that whole record side: the validity query (`isHotValid()`/
`isColdValid()` on `TrackView`, `isValid()` on `ListView` and
`FileManifestView`) reports false and every accessor returns a zero/empty
value. There
are no field-level silent fallbacks that could mask writer bugs, and no
per-field throw that would turn one bad row into an application fault.
`FileManifestStore::Reader::get` additionally reports a short record as
`CorruptData` at its store boundary because it already returns `Result`.

**Semantic corruption within bounds is tolerated at read time.** Unsorted
custom keys, nonzero padding or reserved fields, out-of-range enum values, and
similar in-bounds garbage read back as garbage values, memory-safely. The
deep verifier (`detail::TrackColdReader`) re-checks every write-side
invariant and is the diagnostic tool for these cases; it serves the CLI
record dump and serialization tests and must never sit on the row read path.

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

## Runtime Layer (`ao/rt`)

The runtime layer (`ao/rt`) is an orchestration façade between the core
subsystems and the application frontends (GTK, CLI). Most failures it surfaces
are *re-classifications*, not originations: every core call it composes has
already classified its own external-vs-internal outcome and returned `Result<T>`
or `std::optional<T>`, so at each composition point the runtime's task is not to
pick a return type but to **re-classify** that inner outcome into one of the
caller's intents. It does still originate failures at its own boundaries -
config semantics (a missing or undecodable config file), third-party exception
translation (ryml, `std::filesystem`), and runtime/UI workflow contract
violations - but those follow the same channel rules below, so re-classification
is the dominant discipline rather than the only one. A failure surfaced to a
runtime caller is exactly one of:

- a normal domain state (absence, no-op, empty result) that the caller treats as
  ordinary flow, modeled as a **value**: `bool`, `std::optional<T>`, an empty
  container, a sentinel id, or a named enum, with no error channel;
- a user-initiated operation failure the caller can report, retry, or cancel,
  returned as **`Result<T>`** carrying code, message, and location;
- an unrecoverable fault or broken invariant the caller cannot handle, raised by
  **throwing `ao::Exception`**.

Which one applies is decided by a single discriminator: does the operation touch
user-supplied external data (a file path, an import payload, a config file), or
only our own already-persisted, already-validated internal state?

- **External-data operations** are a genuine external boundary and return
  `Result<T>`, propagating every error code unchanged: `ConfigStore::ensureLoaded`
  / `flush`, `LibraryYamlImporter::importFromYaml`, and
  `LibraryYamlExporter::exportToYaml`. A non-`NotFound` code here is a legitimate
  external-input failure and stays in the `Result` for the caller to render.
- **Internal-state operations without a `Result` channel** read or mutate our own
  store and collapse only `Error::Code::NotFound` to a value-channel meaning -
  `std::nullopt`, `false`, or a sentinel. Every other code is a fault that must
  throw `ao::Exception`, preserving the original `error.location`. This covers
  `LibraryReader` read models and legacy/simple `LibraryWriter` mutators such as
  `deleteList`/`deleteTrack`, where absence is the only expected domain miss.
- **User-facing mutation operations with validation or storage diagnostics**
  return `Result<T>`/`Result<>` and propagate every recoverable code unchanged.
  `LibraryWriter::updateMetadata` and `editTags` return reply values through
  `Result<T>` so serialization, storage, and commit failures are reported without
  escaping as `ao::Exception`. `createList` and `updateList` validate
  user-authored list drafts (`FormatRejected` smart filters, invalid parent
  relationships), and `updateList` reports a stale id as `NotFound` rather than
  `Result<bool>`. Successful no-op updates return an empty `Result<>` or a reply
  with no mutated ids.

The throw in the internal-state rule is specifically how an inner store `Result`
collapses when the façade method has *no* `Result` channel to carry it. When the
enclosing operation does return `Result` - an external-data operation such as
`exportToYaml` or a validated mutation such as `updateList` - an inner
non-`NotFound` store failure propagates unchanged through `std::unexpected` and
is never thrown. The discriminator picks the channel for the *operation*; each
inner-`Result` collapse then follows that channel.

A single operation may also span both boundaries. `LibraryWriter::createTrackFromFile`
returns `Result<TrackId>` for the whole import: external input failures such as
missing/out-of-root files, unsupported formats, malformed tags, or unreadable
file attributes stay in the `Result`, and duplicate manifest entries report
`Conflict`. Internal store failures during `prepare`, `createHotCold`, manifest
`put`, or `commit` also propagate through the same `Result` channel because the
enclosing operation has one. The input-phase filesystem calls must use the
`std::error_code` overloads so a vanished file becomes an `IoError` or `NotFound`
result rather than escaping as an unconverted `std::filesystem::filesystem_error`.

Asynchronous tasks keep the same recoverable-vs-invariant split across executor
hops. `LibraryTaskService::importLibraryAsync` / `exportLibraryAsync`,
`buildScanPlanAsync`, and `applyScanPlanAsync` return
`async::Task<Result<T>>` for recoverable lower-layer failures, so the
application leaf can log the structured `Error` and choose UI text without
parsing an exception message. Exceptions remain only an unexpected-fault
transport: if an invariant failure escapes the worker side, the task carries it
with `std::exception_ptr` and rethrows it on the callback executor, where the UI
logs it as an internal error.

Lifetime-bound coroutine cancellation is neither a recoverable failure nor an
internal fault. `ao::async::OperationCancelled` is the async layer's control-flow
exception for cancelled work; it exists to unwind the coroutine frame so code
after a cancellation checkpoint cannot touch captured objects whose lifetime is
no longer guaranteed. The async layer also recognizes Boost.Asio's bare
`operation_aborted` exception as cancellation, but application code must not
branch on Boost error codes directly. Business code must let cancellation
propagate. A broad `catch (std::exception)` or `catch (...)` inside a coroutine
must first call `ao::async::rethrowIfOperationCancelled(...)`; only the
`LifetimeScope` completion boundary may silently consume cancellation. Cleanup
for a cancelled UI operation belongs to the cancellation initiator or owner
teardown path, not to the cancelled coroutine.

### Playback Failure Events

Playback uses two recoverable channels with different ownership. Synchronous
command rejection returns through the command result: `PlaybackService::play`
returns `Result<>` when playback cannot even be accepted, such as no selected or
ready output route. Asynchronous lifecycle failures that happen after a command
has been accepted are not retrofitted into that return value; `audio::Engine`
emits a typed playback failure, `audio::Player` marshals it under the existing
executor callback contract, and `rt::PlaybackService` exposes it through
`onPlaybackFailure`.

The failure taxonomy is deliberately small and policy-oriented:

| Kind | Meaning | Queue policy |
| --- | --- | --- |
| `TrackOpen` | The input could not be opened, probed, or prepared. | Skip-eligible. |
| `Decode` | The current track failed while decoding after playback had started. | Skip-eligible. |
| `RouteActivation` | The output route could not be opened or configured for playback. | Stop playback. |
| `DeviceLost` | The active backend/device disappeared or entered an unrecoverable stream state. | Stop playback. |

The audio layer owns detection only. `Engine::PlaybackFailure` carries the kind,
the caller-supplied playback item id, the narrow `PlaybackInput`, the engine
generation, the original `Error`, and a recoverability flag. It never branches
on user-facing message text. Runtime owns reporting: `PlaybackService` maps the
item id back to the current or prepared request, attaches `TrackId`,
`sourceListId`, and display title where available, refreshes transport state,
and emits the service signal. If no subscriber handles a `TrackOpen`/`Decode`
failure, the service posts or updates one default error notification keyed by
kind and track. When a subscriber is present, recoverable track failures are left
to that policy owner so an active queue can publish one "Skipped N unplayable
tracks" summary instead of one toast per bad file. Output failures
(`RouteActivation`/`DeviceLost`) always keep the service-level sticky
notification. The service does not decide whether a queue should advance.

Queue recovery belongs to `uimodel::PlaybackQueueSession`, because it already owns
queue successor semantics. During active queue playback, `TrackOpen` and
`Decode` failures advance to the next queue item and update a warning
notification such as "Skipped 2 unplayable tracks". Three consecutive
skip-eligible failures stop playback and post a sticky error so a broken folder
cannot loop indefinitely. A successful now-playing commit or natural idle
advance resets the consecutive-failure count. `RouteActivation` and
`DeviceLost` stop the queue immediately; skipping tracks cannot repair an output
route.

`linux-gtk` is an application leaf: it consumes lower-layer `Result`, value, and
exception contracts into notifications, dialogs, and logs. It does not convert a
recoverable `Result` into `ao::Exception` merely because the operation crossed an
async boundary.

Conversions across these boundaries are lossless and centralized. Do not invent a
runtime-specific exception type for domain failure: no downstream catch site
branches on the structure of an unrecoverable runtime fault, so a plain
`ao::Exception` whose message and threaded-through `Error::location` reach the
log is sufficient, and the recoverable channel that *does* need structure already
exists as the `Error` carried by a `Result`. `OperationCancelled` is separate
async control-flow, not a domain error transport. When converting a store
`Result` to a throw, forward the original `error.location` (`throw
Exception{message, error.location}`) instead of recapturing the helper's own call
site, so the deepest origin survives for diagnostics. The shared
`storageValueOrNullopt` helper is the read-side embodiment of this rule:
`NotFound` becomes `std::nullopt`, any other code throws.

Three invariants enforce the model:

1. No runtime `if (!result)` may take the miss branch without inspecting
   `error().code`. Testing a store `Result<T>` as a plain boolean and treating
   any failure as absence is forbidden; only `NotFound` is absence.
2. An *effective* internal-state mutation is atomic-or-throw: a fault abandons
   the transaction without committing instead of committing the successful
   subset, so a batch never silently drops a failed item while reporting success.
   A stale id or a no-effective-change pass is not a mutation and returns
   `false`/empty/sentinel rather than throwing.
3. No third-party exception (ryml, `std::filesystem`) crosses a runtime API
   boundary unconverted: inside an external-data operation it becomes a `Result`;
   otherwise it is rethrown as `ao::Exception`.

A mutation moves to `Result` only when partial, per-item success is a real
physical outcome the caller must report item by item. Because internal-state
mutations are transactional and atomic, that situation does not arise inside a
single batch command - it is a different operation (validate-then-commit) with
its own `Result` contract, not a reason to weaken the batch command above.

## UI Model Layer (`ao/uimodel`)

The UI model layer is platform-neutral presentation and interaction policy. It
does not own durable state and should not invent subsystem-specific exceptions.
Most APIs are pure value transforms: invalid or absent presentation data becomes
the narrowest value the widget needs (`false`, empty text, an empty collection, a
fallback preset), not a recoverable `Error`.

Use `Result<T>` only at explicit user-input parse boundaries where the caller
must show validation text and keep editing: track-field text parsing returns
`FormatRejected`, and inline-edit workflow converts that result to a
`ParseRejected` value outcome. Advisory UI heuristics, such as presentation
recommendation from a filter expression, may treat parse failure as "no
recommendation" and choose a fallback because the user action being performed is
not query execution.

Persistence helpers in this layer are adapters over `rt::ConfigStore`, not owners
of the file boundary. If the helper returns `Result` (for example layout loading),
it propagates the config error unchanged. If the caller's contract is explicitly
best-effort preferences (for example keymap load/save and GTK layout preset
fallback), the helper may log and return defaults/no-op, but it must inspect the
inner `Error::Code` before choosing the value path. `NotFound` is a normal
"defaults only" value; malformed config, IO failures, and flush failures are
logged diagnostics unless the UI surface has a concrete place to report them.

Internal model invariant failures may throw `ao::Exception` directly through
`ao::throwException` (for example an invalid shipped default key chord). Ordinary
coercion of layout values must not use exception control flow: malformed strings
coerce to the supplied default using non-throwing parsing, while allocation or
logic faults are left as faults rather than laundered into defaults.

## Current Migration

The subsystem status below records the intended boundary for areas that recently
changed or still have local containment rules:

| Area | Status |
| --- | --- |
| `ao/tag` | Public entry points return `Result`: `TagFile::open` reports mapping errors as `IoError`, unsupported extensions as `NotSupported`, and successful opens as non-null owners; `TagFile::loadTrack` reports malformed metadata as `CorruptData`. Per-format `loadTrackImpl()` parsers return `Result<TrackBuilder>` and translate contained corruption-detection exceptions at the format boundary. Parsers raise `CorruptData` through `detail::throwTagError`, and the shared `ao::media` parsing layer through `throwMediaError`; each format boundary catches those leaves and converts them to `Result` — flac catches both `detail::TagException` and `media::detail::MediaException`, while mp4/mpeg catch only `TagException` (their paths raise nothing else). `mpeg::FrameView` table accesses use `operator[]` because the field-encoding ranges match the table dimensions. MP4 audio-property extraction is best-effort: malformed or non-version-0 `mdhd` atoms are skipped, while required metadata still reports corruption through the normal boundary. |
| `ao/media` byte views | Public decode/tag entry points return `Result`; `mp4::Demuxer::parseTrack` catches `detail::MediaException` and translates it to `Result`. MP4 sample-table/timing helpers raise `FormatRejected` via `throwMediaError` (rather than returning `bool`) to keep specific diagnostics for malformed container data. FLAC block iteration validates the current block size before honoring the last-block marker, so a truncated final block reports `CorruptData`. |
| `ao/lmdb` | Fallible setup/write factories return `Result`: duplicate create is `Conflict`, exhausted integer append key space is `ResourceExhausted`, other failures are `IoError`. Point reads collapse to their narrowest shape because absence is their only recoverable outcome: `get` returns `std::optional<std::span<...>>` (empty means absent), `maxKey` returns the largest key or `0`, and `Writer::del` returns `bool`. They never surface a recoverable code — a non-`MDB_NOTFOUND` failure throws, because the corruption that would produce `MDB_CORRUPTED` equally surfaces as `SIGBUS` through the mmap, which no `Result` can intercept. Use the factory functions (public constructor bridges are not kept). Cursor EOF is normal; unexpected cursor failures throw, as does writer use after its transaction commits. |
| `ao/library` persisted views/stores | Read shapes follow the actual failure surface: absence-only lookups return `std::optional<T>` (`ListStore`/`TrackStore`/`ResourceStore::get`); lookups that also validate the persisted record return `Result<T>` (`MetadataStore::load`, `FileManifestStore::Reader::get`). Misses are `NotFound`, corrupt or unsupported-version records are `CorruptData`, oversized manifest URIs are `ValueTooLarge`, exhausted resource IDs are `ResourceExhausted`; `MusicLibrary::open` reports setup failures without throwing. Create/update/clear commands return `Result<>`/`Result<T>` (never `.value()`); deletes return `bool` because corruption throws at the LMDB layer and absence is not an error, except idempotent manifest removal (`Result<>`, since URI validation can reject the key first). Once a session is open, `read/writeTransaction()` treat begin failure as severe and throw. `TrackBuilder` serialization raises its `ValueTooLarge` overflow checks via `detail::throwLibraryError`, translated to `Result` at the `prepare*`/`serialize*` boundaries. Binary layout views follow [Persisted Binary Layouts](#persisted-binary-layouts): constructors run an O(1) structural gate and poison the view instead of throwing, accessors are non-throwing and return zero/empty values for a poisoned side, and at-style accessors guard preconditions with contracts (`gsl_Expects`). |
| `ao/yaml` | Ryml callback exceptions are containment only: they may be used to satisfy ryml's callback API, but runtime/config/import boundaries convert them to `Result`. File reads use `IoError`; malformed YAML syntax, unsupported schema, wrong node kind, malformed scalar text, and out-of-range numeric values use `FormatRejected`. Scalar helpers are non-throwing and strict: numeric reads require full consumption and range fit, and bool reads accept canonical `true`/`false` text. A YAML tree that may outlive parsing owns a `CallbackContext` so later ryml diagnostics never point at a dangling filename. |
| `ao/rt` runtime layer | Governed by [Runtime Layer](#runtime-layer-aort). The runtime *re-classifies* core `Result`/`std::optional` outcomes rather than originating failures. External-data and user-facing validated operations (`ConfigStore`, `LibraryYaml{Importer,Exporter}`, `LibraryWriter::createTrackFromFile`, `updateMetadata`, `editTags`, `createList`, `updateList`) return `Result` and propagate every code; `updateList` uses `Result<>` with `NotFound` for a stale list id. `LibraryScan` is the synchronous scan facade used by frontends; it forwards runtime-private `ScanPlanBuilder::buildPlan` and `ScanApplyOperation::run` results without changing the storage error contract. `ScanPlanBuilder::buildPlan` returns `Result<ScanPlan>`: per-file problems stay in-band as `ScanClassification::Error` items so the rest of the plan still applies, but a failure that prevents any plan at all - a missing music root (`NotFound`) or a filesystem walk that cannot start (`IoError`) - fails the whole call, so the caller reports it instead of mistaking an unscannable root for an empty, up-to-date library. The builder and apply operation are runtime-internal; callers use `LibraryScan` or `LibraryTaskService`. Internal-state reads (`LibraryReader`) and no-`Result` mutations (`LibraryWriter::deleteList`/`deleteTrack`) treat only `NotFound` as a value (`std::optional`/`false`/`sentinel`) and throw `ao::Exception` on every other fault. `LibraryTaskService` returns `async::Task<Result<T>>` for recoverable async failures and uses exception transport only for unexpected faults rethrown on the callback executor. Conversions forward `error.location`; `storageValueOrNullopt` is the read-side helper. |
| `ao/uimodel` UI model layer | Governed by [UI Model Layer](#ui-model-layer-aouimodel). Pure presentation APIs return values/fallbacks; explicit text-parse boundaries return `Result` with user-facing validation messages; config-backed preference helpers either propagate `rt::ConfigStore` results unchanged or are explicitly best-effort defaults with logging. Internal shipped-policy violations may throw `ao::Exception`; ordinary value coercion uses non-throwing parsing and never catches broad exceptions to produce defaults. |
| `ao/query` | Parser, query/format compiler, and field-resolution entry points return `Result`, using `FormatRejected` for user-facing diagnostics. Completion-only field probing uses `std::optional<Field>` because absence is the only needed state. The private compile helpers raise `FormatRejected` via `detail::throwQueryError`, translated back to `Result` by a single catch at `QueryCompiler::compile()`/`FormatCompiler::compile()`. `OperatorTable::operatorDescriptor()` uses `operator[]` with a `gsl_Expects` guard instead of `.at()`, making an out-of-range operator an invariant failure rather than a recoverable `std::out_of_range`. |
| `ao/audio` | Decoder, source, and backend entry points return `Result` for every external failure (`DecoderSession::open`/`seek`/`readNextBlock`, `PcmSource::seek`, `Backend::open`/`setProperty`/`property`); the layer never throws on external input and never collapses a fault to a codeless `Generic`. The `DecoderSession` and `PcmSource::seek` boundary methods are `noexcept`: the contract that this layer never throws to its caller is enforced at compile time (an override that drops `noexcept` is ill-formed) and fail-fast at runtime (an escaping exception such as `std::bad_alloc` terminates instead of being silently reinterpreted as a decode error). Within decoder session implementations (such as `Mp3DecoderSession`), each public method keeps a single internal style: failures are raised with `detail::throwDecoderError` — the helper and the `detail::DecoderException` it throws are an implementation detail, defined in `ao/audio/detail/DecoderError.h` (the `ao::audio::detail` namespace), not part of the public decoder API — and one `try`/`catch (detail::DecoderException const&)` translates them to `Result` at the boundary — a body that calls no throwing helper (such as `AlacDecoderSession::readNextBlock`) stays purely `Result`-returning rather than wrapping a no-op `try`. Errors propagated from an inner `Result` keep their original diagnostic location via `detail::throwDecoderError(Error)`; freshly originated failures capture the call site via `detail::throwDecoderError(code, message)`. The catch is deliberately narrow (only `DecoderException`) so non-domain faults are not laundered into a recoverable code. End of stream is modeled as a value, not an error: `readNextBlock` returns `Result<PcmBlock>` whose `endOfStream` flag marks the final block, and `StreamingSource::decodeNextBlock` returns a `DecodeBlockStatus` enum for its normal success states, signalling decode failure by throwing `DecoderException` to the source's translating boundary. This subsystem uses the media-domain codes from `Error.h`: `InitFailed` for decoder/codec/device initialization, `DecodeFailed` for decode-time payload failures, `SeekFailed` for seek failures, `DeviceNotFound` for an absent device, alongside `NotSupported`/`FormatRejected`/`IoError` for unsupported formats and container IO. `createDecoderSession` returns `Result<std::unique_ptr<DecoderSession>>` — `NotSupported` for an unsupported extension or unrecognized container codec, `IoError` for an unreadable container — instead of a `nullptr` that conflated "cannot play" with "cannot open". `TrackSession` threads the concrete `Error` through format negotiation and PCM-source creation (`NotSupported` when the device cannot accept the stream format) rather than flattening it to a message-only `bool` out-parameter. `BackendProvider::shutdown()` is `noexcept` by contract. |

New code should follow the target rule even when working near legacy code.
