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

Public decoder APIs live under `include/ao/audio/` and return `ao::Result`.
Private translation support lives in `include/ao/audio/detail/DecoderError.h`.
The exact common error vocabulary belongs to the [error value reference](../failure/error.md).

## Factory surface

`createDecoderSession(path, outputFormat)` returns a non-null session or an error.

| Condition | Code |
| --- | --- |
| `.flac`, `.mp3`, or `.wav` extension | matching session value |
| `.m4a` or `.mp4` with `alac` sample entry | ALAC session value |
| `.m4a` or `.mp4` with `mp4a` sample entry | AAC session value |
| supported MP4 extension cannot be mapped | `IoError` |
| supported MP4 extension has no audio track | `NotSupported` |
| unsupported MP4 audio sample entry | `NotSupported` |
| malformed MP4 structure encountered before audio-track selection | propagated `CorruptData` or `FormatRejected` |
| unsupported extension | `NotSupported` |

Extension matching is ASCII case-insensitive.
MP4 route selection stops after the first usable audio track and does not validate unrelated later siblings.
The factory does not open non-MP4 decoder sessions; open-time media validation belongs to the returned session.
WAV session open uses the RIFF parser's `RequiredAudio` extent and therefore does not surface malformed chunk boundaries after the first complete supported `fmt` and non-empty `data` pair.

## Session operation surface

| Operation | Success/normal value | Recoverable code families |
| --- | --- | --- |
| `open(path)` | open session and complete `DecodedStreamInfo` | propagated `IoError`, `CorruptData`, or `FormatRejected`; codec `InitFailed`, `DecodeFailed`, or `NotSupported` |
| `seek(offset)` | decoder positioned for the requested offset | `SeekFailed`, plus a lower packet-source error when the codec delegates seeking |
| `readNextBlock()` | PCM block or empty end-of-stream block | `DecodeFailed`, `NotSupported`, or a propagated lower read error |
| `close()` | closed, empty stream info | none; `noexcept` |
| `flush()` | codec buffers reset when supported | none; `noexcept` |
| `streamInfo()` | current value, empty while closed | none; `noexcept` |

Codec-specific open-time narrowing includes:

- unsupported output sample representation, valid bits, resampling, remapping, or planar layout: `NotSupported`;
- codec or external decoder initialization/configuration failure: `InitFailed`;
- malformed accepted stream encountered during initial decode/metadata processing: `DecodeFailed`;
- AAC/ALAC packet-source `FormatRejected` during open is translated to `InitFailed` after the MP4 codec route has already been selected.

End of stream is `PcmBlock{.endOfStream = true}` rather than an error.
An unopened or closed session also returns a stable empty end-of-stream block when the codec implementation has no readable stream.

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
auto session = ao::audio::createDecoderSession(path, requestedFormat);
if (!session)
{
  return std::unexpected{session.error()};
}
```

## Implementation authority

- [`DecoderFactory.cpp`](../../../lib/audio/DecoderFactory.cpp) owns extension and MP4 sample-entry routing.
- [`DecoderSession.h`](../../../include/ao/audio/DecoderSession.h) owns the common public operation surface.
- [`DecoderError.h`](../../../include/ao/audio/detail/DecoderError.h) owns private translation values and helpers.
- Codec session implementations under [`lib/audio/`](../../../lib/audio/) own their operation-specific messages and lower propagation.

## Test authority

- [`DecoderFactoryTest.cpp`](../../../test/unit/audio/DecoderFactoryTest.cpp) protects routing and factory codes.
- [`DecoderErrorTest.cpp`](../../../test/unit/audio/DecoderErrorTest.cpp) protects helper location preservation and private exception values.
- Codec tests under [`test/unit/audio/`](../../../test/unit/audio/) protect per-operation failure codes and end-of-stream values.

## Related documents

- [Decoder session specification](../../spec/playback/decoder-session.md)
- [Outcome channel specification](../../spec/failure/outcome-channel.md)
- [Error value reference](../failure/error.md)
