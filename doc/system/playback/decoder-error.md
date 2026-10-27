---
id: playback.decoder-error
---
# Decoder error reference

## Scope and version

This reference enumerates the current C++ decoder error surface.
It is not a serialized format, and error-code enum ordinals are not compatibility identifiers.
Lifecycle and recovery semantics belong to the [decoder session specification](decoder-session.md).

## Code boundary

The public ready-session interface lives under `include/ao/audio/` and returns `ao::Result` from fallible operations.
Source-private session construction lives under `lib/audio/`; private translation support lives in `lib/audio/detail/DecoderError.h`.
Concrete decoder constructors and their one-shot `initialize()` methods are private.
Their static `open(path, outputEncoding)` factories return owning ready sessions or errors; no concrete instance exposes `open()`, `close()`, or reopen.
The exact common error vocabulary belongs to the [error value reference](../failure/error.md).

## Factory surface

`openDecoderSession(path, outputEncoding)` returns a non-null, fully initialized session or an error.

| Condition | Code |
| --- | --- |
| `.flac`, `.mp3`, `.opus`, or `.wav` extension with valid supported media | ready matching session value |
| `.m4a` with a valid supported `alac` sample entry | ready ALAC session value |
| `.m4a` with a valid supported `mp4a` sample entry | ready AAC session value |
| `.m4a` cannot be mapped | `IoError` |
| `.m4a` has no audio track | `NotSupported` |
| unsupported MP4 audio sample entry | `NotSupported` |
| malformed MP4 structure encountered before audio-track selection | propagated `CorruptData` or `FormatRejected` |
| selected decoder initialization fails | propagated `IoError`, `CorruptData`, or `FormatRejected`; codec `InitFailed`, `DecodeFailed`, or `NotSupported` |
| unsupported extension | `NotSupported` |

Extension matching is ASCII case-insensitive.
The `.mp4` extension is unsupported; Aobus accepts MP4 audio only through the music-specific `.m4a` extension.
MP4 route selection stops after the first usable audio track and does not validate unrelated later siblings.
Construction initializes the selected concrete decoder before returning it; initialization failures are returned instead of publishing a partial session.
WAV construction uses the RIFF parser's `RequiredAudio` extent and therefore does not surface malformed chunk boundaries after the first complete supported `fmt` and non-empty `data` pair.

## Session operation surface

| Operation | Success/normal value | Recoverable code families |
| --- | --- | --- |
| `seek(offset)` | decoder positioned for the requested offset | `SeekFailed`, plus a propagated packet-source or frame-decoding error encountered during seeking |
| `readNextBlock()` | PCM block or empty end-of-stream block | `DecodeFailed`, `NotSupported`, or a propagated lower read error |
| `flush()` | codec buffers reset when supported | none; `noexcept` |
| `streamInfo()` | complete value established by successful construction | none; `noexcept` |

Codec-specific initialization-time narrowing includes:

- unsupported output sample representation, valid bits, resampling, remapping, or planar layout: `NotSupported`;
- codec or external decoder initialization/configuration failure: `InitFailed`;
- malformed accepted stream encountered during initial decode/metadata processing: `DecodeFailed`;
- AAC/ALAC packet-source `FormatRejected` during initialization is translated to `InitFailed` after the MP4 codec route has already been selected.

FLAC rejects missing STREAMINFO during construction and discontinuous delivered sample positions before returning incorrectly indexed PCM, including when the total sample count is unknown.
A raw-frame write before STREAMINFO preserves the required-STREAMINFO diagnostic without touching an unconfigured output adapter.
The same cause is recorded for an error in native `READ_FRAME` during the initial step before STREAMINFO, including implicit rate/precision headers or raw-subframe `LOST_SYNC` that never reach a write callback.
That cause makes the next read callback abort and takes precedence over generic native-false or decode-error diagnostics; already buffered bytes may still be scanned before the next read.
Normal prefix `LOST_SYNC` remains tolerated, and end of stream alone is not a missing-STREAMINFO cause marker.
Without the recorded cause, an actual native metadata failure such as truncated STREAMINFO retains `Failed to read FLAC metadata`; a failed native processing call remains a hard failure even without an error callback.
The [initialization failure contract](decoder-session.md#failure-behavior) owns these phase and precedence rules.
Its [source-identity policy](pcm-format.md#flac-source-interpretation) treats absent exact mask keys for five/six channels, malformed recognized channel-mask declarations, conflicting duplicates, zero/partial/excess/reserved/wildcard assignments, nonfirst STREAMINFO, incomplete comments, and optional position/marker/cross-check inability as unknown identity, not new `NotSupported` or `DecodeFailed` gates.
Otherwise-valid audio may open with aligned zero source/output masks and `Unknown` origin; later valid declarations never repair that inspection.
Required I/O, native metadata, STREAMINFO, frame, timeline, and premature-EOF faults retain their error behavior, including actual outer metadata truncation rejected by native processing.
Requested-output precision errors remain required failures with their existing precedence over final STREAMINFO geometry validation; optional unknown evidence never hides them.
Unknown playback is best-effort admission, not proof of safe speaker roles or native routing, and does not change numerical precision guarantees.
It preserves a frame-format `DecodeFailed` from a frame delivered to the session while seeking instead of replacing it with generic `SeekFailed`.
It also preserves a recorded cause when a later seek cannot restart an aborted or seek-error codec without an explicit `flush()`; reads in those states fail instead of retrying without progress.
The diagnostic identifies the inspected and conflicting frame properties; a seek failure that has no stored decoding cause remains `SeekFailed`.

### ALAC required errors versus optional identity

ALAC retains required MP4 track, sample-table, and extradata errors and their existing initialization translation above.
Its mandatory cookie must still contain the compact `alac` header and complete configuration; malformed configuration, invalid frame length/version/bit depth/channel count/rate, or provider initialization failure remains `InitFailed`.
The validated ALAC configuration owns geometry and precision, not sample-entry geometry or optional `chan` fields.
Requested unsupported or precision-losing output remains `NotSupported` at output-adapter configuration; no partially initialized session is published.

Optional layout inspection adds no rejection gate.
Malformed, unsupported, conflicting, ambiguous, or uninspectable optional metadata yields zero mask and `Unknown` interpretation, preserving legacy ordinal PCM rather than inventing roles or returning a layout error.
This includes exact colliding tag `0x00710003`, conflicting declaration order at an equal mask, and an unavailable associated-entry view.
A later good declaration never repairs an earlier bad one.
The [ALAC declaration reference](../../reference/media/channel-order.md#alac) owns the bounded grammar and completeness rules.

This is not parser relaxation: invalid atoms encountered on the existing required cookie/container lookup path still fail.
A malformed or nested size-zero optional tail not previously visited by required parsing withholds identity only.
Neither metadata failure nor metadata absence bypasses required cookie/configuration/provider checks or precision-loss rejection.
Read-time provider failures and invalid decoded frame counts remain `DecodeFailed`; normalization does not replace the numerical decoder.

End of stream is `PcmBlock{.endOfStream = true}` rather than an error.
Unopened and closed states are not part of the public `DecoderSession` surface.

## Private translation surface

`ao::audio::detail::DecoderException` carries one recoverable `Error` inside decoder/source implementation code.
`throwDecoderError(Error)` preserves the existing diagnostic source location.
`throwDecoderError(code, message, location)` captures its caller unless an explicit location is supplied.

Public translating methods catch only `DecoderException` and return its error.
They do not catch allocation, logic, invariant, or unrelated exceptions.
Public `DecoderSession` methods are `noexcept`; an unrelated escaping exception therefore terminates rather than being reclassified as external-media failure.
FLAC callbacks are `noexcept`; the allocating write callback contains unexpected exceptions through [exception-aware fatal handling](../failure/fatal.md#exception-aware-entry), not C++ exception transport through the provider.
The metadata callback handles STREAMINFO with inline nonallocating `noexcept` state updates, without the allocating write callback's fatal wrapper or a VORBIS_COMMENT request.
The first native `process_single` step must deliver first-block STREAMINFO before optional identity inspection is eligible; native positions, bounds, and the mapped STREAMINFO header are cross-checks, not sole proof of origin.
Bounded mapped comment parsing happens outside foreign callbacks through the raw allocation-free Vorbis comment walker shared with library import, without owning copies of each comment or a fixed comment-count cap.
Import retains its independent atomic-publication and trailing-byte policies.
Moving this work does not make allocation failure recoverable: the existing one-shot `noexcept` initialization boundary still fails fast for an unrelated escaping exception, including out-of-memory failure.
Raw-frame-phase missing-STREAMINFO detection records inline state in the error callback and aborts at the next read callback; it does not format an owning diagnostic inside either callback.
Frame-format and sample-position failures record inline evidence and their detection locations; owning diagnostics are formatted only after the libFLAC call returns.
Allocation-free comment traversal does not imply allocation-free initialization or whole-stream decoding.

## Validation rules

- Callers branch on code and operation, never diagnostic message text.
- A lower error propagated without semantic translation retains its location.
- A method that invokes no throwing decoder helper returns `Result` directly rather than adding a catch-all boundary.
- `nullptr` is not an alternate factory failure channel.

## Compatibility and versioning

The table describes current source-level behavior.
Changing an operation's code family requires updating its specification and focused tests; no numeric or message stability is promised.

## Examples

```cpp
auto session = ao::audio::openDecoderSession(path, requestedEncoding);
if (!session)
{
  return std::unexpected{session.error()};
}
```

## Implementation authority

- [`DecoderSessionBase.h`](../../../lib/audio/detail/DecoderSessionBase.h) and the concrete codec implementations own factory-only construction, private initialization, and destruction of failed candidates.
- [`DecoderFactory.h`](../../../lib/audio/DecoderFactory.h) and [`DecoderFactory.cpp`](../../../lib/audio/DecoderFactory.cpp) own extension routing, MP4 sample-entry routing, and successful concrete-to-`DecoderSession` conversion.
- [`DecoderSession.h`](../../../include/ao/audio/DecoderSession.h) owns the common public operation surface.
- [`DecoderError.h`](../../../lib/audio/detail/DecoderError.h) owns private translation values and helpers.
- Codec session implementations under [`lib/audio/`](../../../lib/audio) own their operation-specific messages and lower propagation.

## Test authority

- [`DecoderFactoryTest.cpp`](../../../test/audio/DecoderFactoryTest.cpp) protects ready construction, routing, and initialization failure codes.
- [`DecoderErrorTest.cpp`](../../../test/audio/DecoderErrorTest.cpp) protects helper location preservation, private exception values, constructor inaccessibility, and the absence of instance `close()`.
- [`AudioFatalProbeProtocol.cpp`](../../../test/fatal/AudioFatalProbeProtocol.cpp) protects the `TrackSession` invariant that an injected `DecoderFactoryFn` success contains a session.
- [`FlacMetadataInitializationTest.cpp`](../../../test/audio/FlacMetadataInitializationTest.cpp) covers raw-frame missing-STREAMINFO initialization diagnostics, implicit-header cases, and valid metadata/prefix and truncated-metadata controls.
- Codec tests under [`test/audio/`](../../../test/audio) protect initialization failures, per-operation failure codes, and end-of-stream values.
- [`AlacDecoderMalformedMp4Test.cpp`](../../../test/audio/AlacDecoderMalformedMp4Test.cpp) retains required ALAC/container failure coverage; optional-identity coverage is listed in the [decoder test map](decoder-session.md#test-map).

## Related documents

- [Decoder session specification](decoder-session.md)
- [Outcome channel specification](../failure/outcome-channel.md)
- [Error value reference](../failure/error.md)
