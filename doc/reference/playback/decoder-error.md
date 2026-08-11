---
id: playback.decoder-error
type: reference
status: current
domain: playback
summary: Enumerates decoder factory, session-operation, end-of-stream, and private translation error surfaces.
---
# Decoder error reference

## Scope and version

This reference enumerates the current C++ decoder error surface.
It is not a serialized format, and error-code enum ordinals are not compatibility identifiers.
Lifecycle and recovery semantics belong to the [decoder session specification](../../spec/playback/decoder-session.md).

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
| `.flac`, `.mp3`, or `.wav` extension with valid supported media | ready matching session value |
| `.m4a` or `.mp4` with a valid supported `alac` sample entry | ready ALAC session value |
| `.m4a` or `.mp4` with a valid supported `mp4a` sample entry | ready AAC session value |
| supported MP4 extension cannot be mapped | `IoError` |
| supported MP4 extension has no audio track | `NotSupported` |
| unsupported MP4 audio sample entry | `NotSupported` |
| malformed MP4 structure encountered before audio-track selection | propagated `CorruptData` or `FormatRejected` |
| selected decoder initialization fails | propagated `IoError`, `CorruptData`, or `FormatRejected`; codec `InitFailed`, `DecodeFailed`, or `NotSupported` |
| unsupported extension | `NotSupported` |

Extension matching is ASCII case-insensitive.
MP4 route selection stops after the first usable audio track and does not validate unrelated later siblings.
Construction initializes the selected concrete decoder before returning it; initialization failures are returned instead of publishing a partial session.
WAV construction uses the RIFF parser's `RequiredAudio` extent and therefore does not surface malformed chunk boundaries after the first complete supported `fmt` and non-empty `data` pair.

## Session operation surface

| Operation | Success/normal value | Recoverable code families |
| --- | --- | --- |
| `seek(offset)` | decoder positioned for the requested offset | `SeekFailed`, plus a lower packet-source error when the codec delegates seeking |
| `readNextBlock()` | PCM block or empty end-of-stream block | `DecodeFailed`, `NotSupported`, or a propagated lower read error |
| `flush()` | codec buffers reset when supported | none; `noexcept` |
| `streamInfo()` | complete value established by successful construction | none; `noexcept` |

Codec-specific initialization-time narrowing includes:

- unsupported output sample representation, valid bits, resampling, remapping, or planar layout: `NotSupported`;
- codec or external decoder initialization/configuration failure: `InitFailed`;
- malformed accepted stream encountered during initial decode/metadata processing: `DecodeFailed`;
- AAC/ALAC packet-source `FormatRejected` during initialization is translated to `InitFailed` after the MP4 codec route has already been selected.

End of stream is `PcmBlock{.endOfStream = true}` rather than an error.
Unopened and closed states are not part of the public `DecoderSession` surface.

## Private translation surface

`ao::audio::detail::DecoderException` carries one recoverable `Error` inside decoder/source implementation code.
`throwDecoderError(Error)` preserves the existing diagnostic source location.
`throwDecoderError(code, message, location)` captures its caller unless an explicit location is supplied.

Public translating methods catch only `DecoderException` and return its error.
They do not catch allocation, logic, invariant, or unrelated exceptions.
Public `DecoderSession` methods are `noexcept`; an unrelated escaping exception therefore terminates rather than being reclassified as external-media failure.

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
- Codec session implementations under [`lib/audio/`](../../../lib/audio/) own their operation-specific messages and lower propagation.

## Test authority

- [`DecoderFactoryTest.cpp`](../../../test/unit/audio/DecoderFactoryTest.cpp) protects ready construction, routing, and initialization failure codes.
- [`DecoderErrorTest.cpp`](../../../test/unit/audio/DecoderErrorTest.cpp) protects helper location preservation, private exception values, constructor inaccessibility, and the absence of instance `close()`.
- [`AudioFatalProbeProtocol.cpp`](../../../test/fatal/AudioFatalProbeProtocol.cpp) protects the `TrackSession` invariant that an injected `DecoderFactoryFn` success contains a session.
- Codec tests under [`test/unit/audio/`](../../../test/unit/audio/) protect initialization failures, per-operation failure codes, and end-of-stream values.

## Related documents

- [Decoder session specification](../../spec/playback/decoder-session.md)
- [Outcome channel specification](../../spec/failure/outcome-channel.md)
- [Error value reference](../failure/error.md)
