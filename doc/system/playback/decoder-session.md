---
id: playback.decoder-session
---

# Decoder session

## Scope

This specification defines the common `DecoderSession` behavior and the current per-codec PCM output capabilities.
File recognition and library codec values belong to the [supported audio files](../../reference/media/audio-file.md) and [track model](../../reference/library/model/track.md) references.

## Code boundary

This contract belongs to the **core libraries** layer in the [system architecture](../overview.md) and refines the decode boundary in the [playback architecture](README.md).
Its public surface lives under `include/ao/audio/` and its implementations live under `lib/audio/`; decoders may consume lower core media and utility facilities but do not depend on application runtime, UIModel, or frontends.
Each source-private concrete decoder is created only through its static `open(path, outputEncoding)` factory.
Its constructor and one-shot `initialize()` step are private, and it exposes no instance `open()`, `close()`, or reopen lifecycle.

## Invariants

- `DecodedStreamInfo::sourceFormat` describes source stream properties, while `outputFormat` truthfully describes returned PCM.
- Every published `DecoderSession` is already open and has complete, valid `streamInfo()`; the public interface has no unopened or closed state.
- A decoder created without a requested encoding inspects the stream and chooses its preferred lossless native representation.
- A decoder created with a requested `SampleEncoding` either produces exactly that interleaved representation or is not published.
- Decoders preserve inspected sample rate, channel count, sample kind, precision, and effective speaker mask; they do not silently resample, add or remove channels, or reduce precision.
- A decoder publishing identified speakers normalizes their codec-native layout to the order owned by the [PCM format reference](pcm-format.md); unassigned channels retain their index order.
  Byte-order normalization alone does not establish published layout evidence; Opus identifies through its mapping-family role assignment, not through the permutation that normalization performs.
- WAV applies its [source interpretation](pcm-format.md#wav-source-interpretation) before output adaptation and retains raw declarations independently; its multichannel and explicit-zero compatibility fallbacks are `Inferred`.
  FLAC finalizes its candidate effective mask before output adaptation and publishes its distinct [source interpretation](pcm-format.md#flac-source-interpretation) only after required initialization and output admission succeed: with complete trustworthy metadata and first-block STREAMINFO, absent exact mask keys select `Identified` format defaults for counts 1/2/3/4/7/8, but zero / `Unknown` for 5/6 because back/side geometry is unresolved.
  Complete supported agreeing declarations select exact `Identified` sets without additional PCM reordering, including distinct five/six-channel back and side sets.
  Malformed, conflicting, zero, partial, excess, reserved-bit, or wildcard declarations, nonfirst STREAMINFO, incomplete comments, and optional inspection inability leave both FLAC masks zero and origin `Unknown`, never `Inferred`; later good evidence cannot repair that inspection.
  ALAC applies its [bounded source interpretation](pcm-format.md#alac-source-interpretation) before output adaptation: trustworthy absence identifies mono/stereo only, and complete supported agreeing declarations identify and normalize explicit layouts; bad or uninspectable optional evidence remains zero / `Unknown`.
  MP3 applies its [bounded source interpretation](pcm-format.md#mp3-source-interpretation) before output adaptation: complete native-index header coverage of a uniform mono or stereo/joint stream is `Identified`, while dual-channel, mixed roles, and incomplete coverage retain zero masks and `Unknown`.
  Opus applies its mapping families as [source interpretation](../../reference/media/channel-order.md#opus): families 0 and 1 own authoritative output-slot roles and publish their channel count's WAVE mask with `Identified` interpretation, while family 255 keeps zero masks and `Unknown` evidence in header-defined ordinal order.
  AAC applies its [actual-output source interpretation](pcm-format.md#aac-source-interpretation) before output adaptation and publication: complete bounded FDK output-role groups identify and normalize slots; unavailable or unsupported roles remain zero / `Unknown`.
- Unsupported or precision-losing encoding requests fail during source-private construction before playback begins.
- An exhausted session returns a stable empty end-of-stream block; destruction ends the session and releases its codec resources.

## Output representations

Every current decoder normalizes its codec-native output through the same precision-preserving PCM adapter.
The resulting set depends on the inspected signal rather than the output device:

| Inspected signal         | Supported output encodings                                                   |
| ------------------------ | ---------------------------------------------------------------------------- |
| Integer, at most 16 bits | Signed 16-bit; packed 24-bit; 24-bit in 32-bit; signed 32-bit; 32-bit float. |
| Integer, 17–24 bits      | Packed 24-bit; 24-bit in 32-bit; signed 32-bit; 32-bit float.                |
| Integer, 25–32 bits      | Signed 32-bit.                                                               |
| 32-bit float             | 32-bit float.                                                                |

The ordered exact enum surface and byte layouts belong to the [PCM format reference](pcm-format.md).
Integer-to-float mapping is bit-transparent only through 24-bit precision.
No decoder advertises or accepts a representation that reduces the inspected signal precision.

AAC, MP3, and Opus expose a 16-bit decoded integer signal for this contract; their encoded sources remain lossy.
FLAC, ALAC, and integer WAV expose their encoded PCM precision, while float WAV retains the floating-point domain.
Opus is internally a floating-point codec, but decoding it to float would make its sessions unopenable for the integer encodings exclusive-mode devices expose, because the PCM adapter refuses every float-to-integer conversion.

## Stream lifecycle and seeking

The playback path first creates a short-lived inspection decoder with no requested encoding.
Successful inspection supplies complete source and preferred output formats without a public `readNextBlock()` call.
AAC construction privately decodes and caches one first packet to establish those facts, including for inspection; inspection is not necessarily a metadata-only operation.
With a valid Backend prewarm hint, explicit-start preparation creates an optimistic final decoder and prepares its source for the initial offset. After backend selection, playback reuses that decoder on a complete PCM-mode match or prepares a fresh source in the selected encoding. Normally preparation includes seek and preroll; an [extended seek](audio-execution.md#streaming-decode-and-buffering) stays pending for the activated source's decode worker.
The backend-selected encoding is always a member of the inspected signal's lossless output set, so the final decoder retains the same precision-preserving contract as optimistic preparation.
Inspection-to-final stability covers the complete effective source format, including the effective speaker mask: a final session whose source format differs from the inspection is `FormatRejected`.
A drift limited to the source layout's evidence origin re-describes the same layout, changes no PCM identity, and is accepted.

Decoder blocks containing PCM remain consumable before a later empty end-of-stream block.
`firstFrameIndex` identifies the actual first PCM frame in a block, including after decoder-level seek adjustment.

Opus decodes at a fixed 48 kHz and reports that rate as its source rate; the
identification packet's input sample rate describes the encoder's source and is
never published.

Every position in an Opus stream is measured against the timeline the container
establishes, which the session derives once and shares with the media-file
reader. A stream cropped at the front or joined from a live source starts at a
nonzero decode origin, and playback starts at that origin advanced past the
header pre-skip, because pre-skip is a length discarded from the decoder output
rather than a region of the absolute timeline. The session discards that
pre-skip before emitting its first frame and stops at the total the timeline
states, so a block sequence carries exactly the audible frames and no
codec-internal padding. A stream whose declared total is zero reaches end of
stream without emitting a block, which is distinct from a stream that never
declared a total and is decoded for as long as it yields packets.

Seeking converts a playback offset into a granule position by measuring from the
playback start, restarts at the earliest packet that can produce a position
80 ms ahead of it, resets decoder state, and discards decoded samples up to the
target, so `firstFrameIndex` names the requested frame exactly. A restart at or
before the first audio packet resumes at the decode origin rather than at zero,
because the header pages carry the granule position zero RFC 7845 fixes for
them, which names no position on a stream that starts cropped. The pre-roll
makes the discarded run long enough for the decoder to converge before the first
audible frame, which matters on streams paged finely enough that a restart would
otherwise land within one packet of the target.

The identification packet's output gain reaches libopus unconverted, which applies the RFC 7845 header gain during decoding.
Channel mapping family 1 orders its channels the way Vorbis does, so the session permutes the mapping it hands libopus into the WAV speaker order every PCM surface expects.
The [Opus source-identity policy](../../reference/media/channel-order.md#opus) determines source/output masks before the immutable stream information is published.

MP3 construction requests a growing native frame index, performs a native scan, and seeks back to sample zero before reporting stream information.
The scan and the return to zero are required; either failure prevents publication.
Initial native frame information is also required.
If it is missing, or if it disagrees with the native PCM rate or channel count, open fails rather than publishing an unassigned fallback.
The scan builds the native seek index and duration even without a Xing or VBRI seek table.
For a [supported trailing-tag chain](pcm-format.md#mp3-source-interpretation), a complete indexed-frame boundary must independently witness its start before the native input can be shortened.
The session then reopens and rescans that audio extent before publication, preserving native duration and gapless calculations while keeping tag bytes out of PCM decoding.
Ambiguous payload markers and malformed tag chains retain the original native input and `Unknown` identity.
Each scan traverses the input; the growing full index adds allocation and retained-memory costs described by the source policy.
A successful native scan alone does not prove complete header coverage; identity inspection checks that separately.
Published identity is the immutable result of complete native-index header coverage, with the initial native prefix exempt, under the [MP3 source policy](pcm-format.md#mp3-source-interpretation).
It is not the first frame alone, and it does not establish corruption-free audio or file authenticity.
Seek rejects negative offsets and offsets beyond a known exact duration.
For ordinary seeks, mpg123 decodes preroll to rebuild the Layer III bit reservoir and synthesis history. With incomplete index coverage, this preroll is bounded and best effort: some free-format streams can require more history than that bound supplies.
If a fully indexed stream mixes Layer III with another layer, mpg123 can limit preroll based on the layer being left. When the frames have a common sample count and their total matches the native PCM length, the session seeks earlier and discards the extra decoded PCM before returning the target.
The session decodes and checks one MPEG frame at a time before exposing PCM, because native gapless trimming can make one read span more than one frame and a bulk read would hide a geometry change.
Gapless header frames yielding no PCM are consumed rather than mistaken for EOF.
In-frame seek skips and gapless trim are applied by mpg123. Public positions count PCM frames, never compressed frames.
The native output block may grow after open.
Signed-16 pass-through copies each span into session storage sized to that span, so a later seek can reallocate the native buffer without invalidating the returned block.
Wider precision-preserving conversions read the native bytes directly and leave converted storage with the shared adapter.
Both preserve the existing [borrowed-block lifetime](../../../include/ao/audio/PcmBlock.h); partial samples and partial frames remain invalid.
A rate, channel-count, or precision change is terminal `NotSupported` and contributes no PCM.
A decoded frame that contradicts a published known role is refused before its PCM is returned.
A same-count stereo/dual mixture is unknown from open, so its PCM is delivered and the mask is never promoted on read or seek.
A layer change that preserves rate, channel count, and precision is tolerated.
A rejected frame does not change published `streamInfo()` or reconfigure the output adapter.
Repeated reads preserve the error, and flush does not clear it.
A successful seek clears the error and the next read revalidates the target; later frames with intact geometry are accepted, while a still-contradicting target is rejected again without adopting a different identity.

## AAC initialization, identity, and seeking

AAC preserves required MP4 track/table/extradata and native AudioSpecificConfig validation before requested-output admission.
After those checks, an AAC-local preflight uses the fixed 16-bit integer signal facts to reject unsupported requested encodings, including `Unknown`, with `NotSupported` before accessing or decoding the first payload.
Required container/cookie/ConfigRaw failures therefore retain precedence over an invalid request, and an invalid request retains precedence over a corrupt first payload.

Initialization privately decodes exactly the first required packet through the same packet pipeline used by public reads, with no search for a later usable packet or concealment fallback.
An empty, inaccessible, corrupt, incompletely consumed, or undecodable payload in the admitted first sample fails `open()` with `DecodeFailed`; there is no ready empty-session fallback.
During sequential playback, a bad later packet fails the read that reaches it. If a seek decodes that packet to rebuild history, the seek itself can report the error.
A silent first block is still valid PCM, and a one-packet stream needs no second packet to become ready.

ConfigRaw may succeed with zero output rate/count/frame size, so neither those provisional fields nor an ASC fallback is the published format.
The first successful frame supplies actual positive geometry, within the supported eight-channel and 2048-frame bounds, and copied ordered output-role evidence.
The [AAC role map](../../reference/media/channel-order.md#aac) resolves a complete plan before the single output-adapter configuration; source and output masks agree at successful open.
Unsupported or unavailable roles yield zero / `Unknown` rather than a layout refusal, partial identity, count default, or `Inferred` fallback.
Known noncanonical slots normalize by moving whole native 16-bit samples before encoding adaptation; canonical known and unknown layouts bypass that permutation.
Decoding uses bounded native scratch storage; normalization and adaptation receive only the active decoded samples, never its unused tail.

Every native handle requests mapping1 and explicitly sets both `AAC_PCM_MAX_OUTPUT_CHANNELS` and `AAC_PCM_MIN_OUTPUT_CHANNELS` to zero, disabling downmix and channel extension; failure to configure these parameters is `InitFailed` during construction or `SeekFailed` on a new seek handle.
This prevents FDK's default eight-to-six-channel downmix from silently becoming the inspected signal when priming discovers actual geometry.
Eight-channel FRONT5 or BACK4 output remains eight-channel PCM with unknown roles, rather than a six-channel mix or a guessed canonical eight-channel identity.
The numerical decoder and ordinary-stream output representation remain unchanged; disabling that default conversion is an explicit channel-preservation change.
No limiter, DRC, concealment, or signal-history flags are added.

The initial rate/count and copied ordered `(type,index)` arrays, including availability of each array, form a fixed native signature.
Every later successfully decoded frame is checked before PCM exposure; supported-geometry rate/count drift or any signature change returns `NotSupported`, including same-mask slot drift and changes to an unknown signature.
Frame size may vary within the supported bound and is not an identity field.
This is a fixed-session support restriction, not a claim that every changing AAC stream is corrupt.
Unknown identity is never promoted, and public stream information is never rewritten by reads, seeks, flush, or errors.

The first converted block is cached in decoder-owned storage before the ready session is returned.
Priming captures its original packet's `firstFrameIndex` and advances the source exactly once.
Public read consumes that cache before checking source EOF, without another decode, source advance, or adapter configuration.
Thus a single-packet file returns its nonempty cached block with EOF before stable empty EOF blocks; repeated information queries do not consume it.

When a seek needs a new handle, it opens and configures it before moving the MP4 cursor. Failure at this step returns `SeekFailed` with its original diagnostic context, without disturbing the current decoder.
Frame positions count native PCM frames from the start of uninterrupted decoding. MP4 packet timestamps can be rounded or absent; they are not PCM boundaries.
For plain AAC-LC at the configured core rate and frame length, the session selects the covering access unit by its fixed native stride and backs up enough packets to rebuild transform overlap and FDK's output delay. This locates the requested frame but need not reproduce every low-order sample bit of an uninterrupted decode.
If FDK later reports dynamic-range control (DRC) without a change to output identity or frame length, the session still returns that packet's PCM at its original position. The declaration rules out short restarts on subsequent seeks.

**Known limitation:** A seek before the decoder has seen any DRC declaration can still use a short AAC-LC restart. If a sparse in-band declaration falls before the restart point, the decoder can miss its gain state and return PCM at a different level from uninterrupted playback. No fixed preroll length guarantees recovery of that state.

A changed frame length after a nonzero short restart contradicts the stride used to infer the packet's position, so the read fails with `DecodeFailed` instead of publishing a speculative frame index.

An SBR extension needs its own signal history even when the core and output rates and frame lengths agree. For history-dependent audio the session retains recent native signed-16 PCM from continuous decoding. A rewind into that window replays the exact samples from the target without moving the native decoder backwards. At the end of the cached PCM, reads continue from the still-positioned decoder. Plain AAC-LC using short restarts does not retain a rewind window.

Each window keeps whole access units and evicts the oldest first. It holds at most 12 seconds of decoded audio, 20 MiB of native sample data, and 2048 access units, whichever limit is reached first. Up to three windows can coexist: the active decoder's, one saved checkpoint's, and a fresh scan candidate's. The limits apply to each window separately, not to total session memory. The time span is measured back from the decoded cursor, including read-ahead, not from the audible position.

A seek can replay either the active or saved window. Otherwise it uses nearby continuous decoder state, scans forward from a suitable saved checkpoint, or starts at the beginning. Throughout the scan, each decoded access unit advances the frame count and enters the rolling window after its output signature is checked. A short rewind after a long seek can replay that recent PCM without repeating the scan. After a history scan for an in-range target, the first read has fewer than 2048 PCM frames left to discard. Plain AAC-LC short restarts instead discard their overlap and delay preroll on read.

An HE-AAC seek validates the access units it needs to reproduce the target PCM, not the whole file. If a required unit is unreadable or fails to decode, the decoder returns `DecodeFailed`; it does not skip that unit, insert silence, or cold-start at a later packet and label the result continuous PCM. A change to the published output identity remains `NotSupported`.
A target after a damaged unit can be unreachable if every exact replay path crosses it. A target reachable from intact cached PCM or valid history without that unit remains seekable; success does not mean later reads will cross the damage. Seeking to the published end does not validate the skipped audio.

Direct `seek()` and `seekInterruptibly()` leave the previous read position unchanged on failure or cancellation, including any pending rewind. `seek()` waits for the work; a cold scan can take time proportional to the seek position. On cancellation, a scan that borrowed the saved checkpoint returns its validated progress to that slot. An unfinished fresh scan can fill an empty slot but cannot displace a saved checkpoint; a fresh scan that finishes just before cancellation can replace it.

`needsSeekWorker()` distinguishes extended scans from bounded or end requests. The playback [decode worker](audio-execution.md#streaming-decode-and-buffering) uses `seekAfterDiscard()` only after the source has retired the old read position and its borrowed PCM. A forward scan can then advance the active native decoder if it has continuous history and no usable saved checkpoint is as close or closer to the target. Otherwise, an extended seek uses a suitable saved checkpoint or decodes from the beginning; this also covers rewinds outside the retained windows.

Older PCM in the active window can age out as the decoder advances. Cancellation retains validated forward progress rather than restoring the relinquished position. If the scan fails, the active handle and its window are no longer eligible for exact seeks because a failed native decode may have consumed input. After cancellation or failure, the source must successfully seek again before reading, or retire the session.

In playback, failure of the current attempt during seek or preroll follows [Engine's source-error policy](audio-execution.md#seek-admission-and-failure); it does not resume the old audio. A superseded worker attempt cannot fail its replacement.
If decoding restarts at zero and the primed block is still pending, the session can reuse that block and handle. The first returned block omits all PCM before the target, including any prefix of the covering packet; `firstFrameIndex` names the first frame actually returned.

An offset at or beyond the positive published MP4 duration makes reads return EOF without moving the native cursor. Both the active and saved rewind windows remain available for a later seek. The published duration is in whole milliseconds: an offset equal to it is an end request even if the exact container duration has a fractional millisecond left. For an earlier offset within the decoded audio, the last packet returns its untrimmed native suffix rather than stopping at container duration. With no published duration, only the native packet extent determines EOF.
These coordinates do not apply edit lists, encoder priming or tail padding.

Healthy `flush()` retains any cached block and only clears compressed input. It is not a seek: it does not reset delayed PCM or transform history.
Priming neither trims AAC delay/padding nor makes AAC gapless-capable.
Native default algorithmic delay remains the selected provider's behavior; the cached first block can contain silence or nonzero PCM, and is preserved in either case.

## ALAC metadata and PCM

ALAC initialization requires successful MP4 track, sample-table, and extradata parsing, compact cookie and configuration validation, and provider initialization.
The initialized ALAC configuration, not sample-entry channel/rate/sample-size fields or `chan`, owns PCM geometry and precision.
Optional channel inspection then resolves the final effective mask and slot plan before the single output-adapter configuration.
Source and output masks agree at successful open, with no first-packet read to discover identity.

The raw [`Demuxer`](../../../include/ao/media/mp4/Demuxer.h) exposes a borrowed ALAC sample-entry view only when that declared entry contains the actual selected-cookie span.
The required cookie lookup can select a later physical entry's cookie even when `stsd` declares one entry.
If association cannot be proved, the demuxer withholds the optional view rather than borrowing from the wrong entry or rejecting playback on that basis.
Optional inspection understands only version-0 sound descriptions.
A missing association or another version leaves identity `Unknown` even for mono/stereo; neither case proves complete absence of channel declarations.
The optional ALAC view does not affect MP4 routing or extradata selection for AAC.
[`Mp4PacketSource`](../../../lib/audio/detail/Mp4PacketSource.h) forwards the view while owning the mapped file; the mapping outlives its demuxer, and borrows expire at close, the next open, or destruction.
Media exposes raw structure only; codec layout policy stays in audio.

Trustworthy complete absence identifies mono FC and stereo FL/FR only.
Absent multichannel roles remain unassigned, and unsupported, ambiguous, conflicting, or uninspectable optional metadata remains `Unknown`, never a WAV/reference count inference.
The [channel-order reference](../../reference/media/channel-order.md#alac) owns the bounded declarations, collision rules, and requirement that duplicates agree on order as well as set.
Any bad declaration is terminal for identity; a later good one cannot repair it.
Unknown identity alone does not prevent a ready session. If required parsing or initialization fails, or the requested PCM output is unsupported or loses precision, [`open()` returns an error](decoder-error.md#alac-required-errors-versus-optional-identity) without publishing a session.

Initialization caches whether the final known slot plan needs permutation; canonical and `Unknown` layouts bypass normalization without rescanning their slots on each block.
For a known layout needing permutation, each decoded block's raw provider-arrival slots normalize to ascending WAVE bits by copying whole 2-, 3-, or 4-byte native samples through fixed per-frame scratch before output adaptation.
Unknown layouts preserve the decoder provider's PCM slot order.
This changes neither the numerical decoder nor seeking, flush, packet progression, frame positions, or stable EOF semantics.
It does not establish native delivery or guarantee every ALAC coding profile.

## Segment playback

A library segment carries a half-open PCM frame range, the carrier's exact total frame count, and its stored positive sample rate in `PlaybackInput`.
Both the inspection and final decoder are wrapped by the segment adapter.
Their source rate and exact extent must match the stored structure before any PCM is exposed; a mismatch is `FormatRejected`.
A decoder without an authoritative extent or exact frame seeking is `NotSupported` for segments, even if it can play the whole file.
FLAC supplies its nonzero STREAMINFO total sample count and absolute-sample seek; WAV supplies its data-frame count and frame-aligned seek.

All adapter coordinates are segment-relative, including `firstFrameIndex`, duration, and frame seek.
A millisecond seek computes the relative frame with integer arithmetic, adds the stored start in frames, and discards any decoder lead-in by its actual frame indices.
It never rounds the segment start through milliseconds.
The exclusive end truncates the crossing block and produces stable EOF until a seek back inside; the last frame remains playable.
An inner EOF before the declared segment end is `DecodeFailed`, including a payload truncated behind an intact header.

The displayed duration is floored milliseconds, not the adapter's end marker.
For 588 frames at 44.1 kHz, a decoder-level seek to 13 ms still delivers the final 15 frames.
A nonempty segment may display zero milliseconds and still plays its frames at offset zero.
The application's explicit end-of-Track seek is a separate [transport policy](application-commit.md#quick-mutations-and-external-settlement), not a weakening of this frame-exact adapter contract.

## Codec and gapless observations

`DecodedStreamInfo::codec` and `isLossy` are conservative.
Unknown codecs remain `Unknown`, and every lossy decoder reports lossy.

The current engine splice gate accepts only lossless FLAC, lossless ALAC, and WAV sessions whose canonical PCM modes satisfy the backend's [installed-transport reuse contract](audio-execution.md#prepared-lookahead).
Exact equality is the default; bounded backend proof may additionally admit stereo mask `0` versus FL/FR `0x3` without changing either track's identity.
Source layout origin is not part of mode equality; successor adoption publishes its own evidence even when an inferred and an identified layout share the same effective mask.
The already-open mode and any retained confirmed endpoint must preserve the successor signal; otherwise the current track drains and a separate successor start needs a fresh open.
ALAC identifies mono as FC and stereo as FL/FR when it can establish that layout metadata is absent.
These roles match ordinary mono and stereo FLAC and WAV for strict reuse; supported explicit ALAC layouts can likewise match canonical identities after slot normalization.
Absent-mask five/six-channel FLAC drains against otherwise-compatible inferred WAV identity, but may reuse a zero-mask mode with untagged multichannel ALAC or unknown FLAC.
These identity matches do not guarantee a splice or native routing. Equal zero masks do not establish either source's original roles.
Lossy and unknown-codec sessions take the drain path even if the decoder already trims delay or end padding. Matching PCM modes would not suffice for future lossy splice support: the decoder would also need to account for codec-specific delay and end padding, with fixtures verifying the decoded PCM across the transition.
An `Unknown` channel layout alone does not exclude lossless FLAC.

## Failure behavior

Malformed external media and unsupported requests return recoverable decoder errors.
FLAC requires STREAMINFO to establish a valid source and output format before construction succeeds; missing required metadata is `DecodeFailed`.
Initialization makes one native `process_single` call before bulk metadata processing; only STREAMINFO delivered by that first step establishes an origin eligible for optional identity inspection.
Native positions, bounds, and the mapped STREAMINFO header only cross-check that origin; normal prefixed first-block STREAMINFO remains identifiable, while native-tolerated nonfirst STREAMINFO stays unknown even with an earlier valid declaration or a fake marker inside padding.
During that initial step, an error in native `READ_FRAME` state before STREAMINFO records a raw-frame-phase, missing-STREAMINFO cause, including when an implicit sample-rate or bit-depth code or a raw-subframe `LOST_SYNC` prevents delivery to the write callback.
The next read callback returns `ABORT`; libFLAC may still scan already buffered bytes before requesting another read, so this is not immediate termination or a strict byte-count bound.
Initialization preserves that recorded required-STREAMINFO diagnostic before generic native-false or decode-error handling.
The write callback also guards raw-frame delivery before STREAMINFO, retaining the same cause without touching an unconfigured adapter.
Normal junk/ID3-prefix `LOST_SYNC` notices remain ignored for native resynchronization; valid STREAMINFO with implicit code-zero frame fields remains ordinary valid input, including 48000 Hz / 16-bit audio.
End of stream alone does not establish the raw-frame cause: an actual native metadata-processing failure without that cause, including truncated STREAMINFO, retains `Failed to read FLAC metadata`.
FLAC must report native metadata, header, frame, CRC, missing-frame, and premature-end failures as `DecodeFailed`, even when a failed native processing call produces no error callback. A standalone loss-of-sync notice may still allow resynchronization.
At native end of input, a recorded callback error takes precedence over a known sample-count shortfall. If the native read failed and neither explains it, the session retains a generic `DecodeFailed` rather than returning clean EOF on a later read or after `flush()` alone.
One exception preserves trailing ID3v1 tags: when the last delivered frame ends exactly at the start of a final 128-byte `TAG` suffix, a failed scan of that suffix alone may end playback normally. The marker does not stop decoding: a later valid frame is still delivered, and neither a callback error nor a sample-count shortfall is waived.
Without a declared sample total, clean EOF does not establish that all original audio was present.
Internally unreadable VORBIS_COMMENT payloads are a distinct compatibility case: the session leaves those blocks unrequested through libFLAC and scans bounded views from the same file mapping outside C callbacks.
Such comment damage alone does not refuse otherwise-valid audio, but prevents publication of a hard source identity even if another readable declaration is valid.
Under the [FLAC source policy](pcm-format.md#flac-source-interpretation), the decoder also leaves speaker identity `Unknown` if it finds an invalid recognized declaration or cannot verify the metadata chain from the native first-block STREAMINFO evidence. That check uses the recorded block position to locate the mapped header, compares the header with the native report, and bounds the chain by the native metadata end.
This best-effort identity scan does not suppress I/O errors, required STREAMINFO failures, actual outer metadata-boundary failures, or native audio errors.
Unknown admission is best-effort transport, not a role-safety or native-routing guarantee; numerical bit-perfectness is independent of channel confidence.
Delivered frame sample positions must continue the decoded timeline, or begin at the requested seek target.
A missing-sample gap or additional write over pending PCM is rejected before any incorrectly positioned block is returned, even when libFLAC suppresses its missing-frame notice or the total sample count is unknown.
A healthy FLAC `flush()` resets provider buffers without skipping unread PCM: the byte cursor is restored to the decoded boundary, and already decoded seek-target PCM remains consumable at its original sample position.
Before emitting PCM, FLAC rejects any frame delivered to the session whose rate, channel count, or bit depth differs from the inspected STREAMINFO properties.
This includes the first frame and the possibly partial block containing a seek target.
This is a fixed-session support restriction, not a claim that every property-changing FLAC stream is invalid.
Seeking does not validate every intervening frame: frames that libFLAC skips or decodes without delivering to the session are outside this per-frame check.
A contradicting frame delivered during seeking produces a preserved `DecodeFailed`; repeated reads retain the failure rather than converting it into end of stream, and `flush()` alone does not clear it.
A seek attempted while the codec is aborted or in seek-error state does not clear a recorded decoding cause; it returns that cause, or `SeekFailed` when none was recorded, until the caller flushes the codec.
Reads in those states return the recorded cause or `DecodeFailed` rather than retrying without progress. A read attempted in seek-error state is not itself a new cause for a later seek.
After a failed read that left the codec at native EOF, an exact seek can replay an intact frame without a flush. Seeking exactly to the known frame count positions the session at empty EOF without validating the skipped audio; rejecting a request beyond that count preserves the read failure.
After flushing the failed codec, a successful decoder-level seek can restart at an intact frame and revalidates every subsequent frame delivered to the session.
Flushing does not repair missing frames: a stream with no decodable seek target still returns a seek failure.
This isolated decoder recovery does not change [Engine's error-state seek admission](audio-execution.md#seek-admission-and-failure), which requires a new playback start.

Decoder operations and streaming-source entry points use `Result` for external media, IO, and capability failures.
The source-private `openDecoderSession` construction boundary returns a non-null ready session or a recoverable error; `nullptr` is not an alternate error channel.
An Ogg stream whose pages carry no complete audio packet is translated to `FormatRejected`, while a malformed page structure or an unusable identification packet preserves its parser error. Because an Ogg page is atomic, a truncation reaching into the first audio page leaves no decodable audio.
An Opus first audio page whose granule position is smaller than the samples it completes is corrupt unless that page also ends the stream, where the smaller position is permitted end trim.
MP4 factory routing stops after selecting the first usable audio track. No matching audio track is translated to `NotSupported`, while a structural parser failure encountered before selection preserves its parser error.
WAV construction validates chunk boundaries only through the first complete supported `fmt` and non-empty `data` pair; unrelated later chunks do not prevent decoding already-bounded audio data.
After existing geometry and precision validation, the [WAV source policy](pcm-format.md#wav-source-interpretation) refuses unsupported layouts with `NotSupported` before output adaptation or backend open; it never rewrites canonical sample order to manufacture compatibility.
After native metadata processing, bounded optional inspection finalizes the FLAC source mask before requested-output adaptation, followed by STREAMINFO geometry validation.
Unsupported or malformed optional identity evidence produces aligned zero source/output masks and `Unknown`, not `NotSupported`; required requested-output precision errors retain their existing precedence over final STREAMINFO geometry validation.
Comment parsing occurs outside foreign callbacks through the allocation-free raw Vorbis comment walker shared with library import; playback does not allocate owning copies of every comment or impose a fixed comment-count cap.
Sharing the walker does not change import's atomic publication or format-specific trailing-byte semantics.
Private output configuration is not session publication: a later required geometry failure destroys the candidate, and every returned session remains ready and one-shot.
FLAC does not apply WAV's zero fallback or excess-bit trimming, and withholding identity is not a general FLAC-validity judgment.
For `play()`, Engine reports WAV layout refusals and required FLAC inspection failures, including malformed outer metadata, as recoverable `TrackOpen` failures without opening or starting the backend.
For `setNext()`, it returns the inspection error synchronously after disarming any unconsumed predecessor lookahead; it does not report a current-playback failure or interrupt the current track in the absence of a competing render transition.
End of stream is a normal `PcmBlock` value.

Implementations may use the private `ao::audio::detail::DecoderException` and `throwDecoderError` helper to unwind codec helpers.
Each public translating method catches only that leaf and preserves a propagated error's diagnostic location.
Allocation, logic, and invariant faults are not reclassified as decode errors.
The `DecoderSession` surface is `noexcept`, so an unrelated escaping exception fails fast.
The exact operation/code matrix and helper surface are in the [decoder error reference](decoder-error.md).

## Implementation map

- [`DecoderSession.h`](../../../include/ao/audio/DecoderSession.h) defines the shared ready-session surface.
- [`DecoderOutputAdapter`](../../../lib/audio/detail/DecoderOutputAdapter.h) and codec implementations under [`lib/audio/`](../../../lib/audio) own the output matrix.
- [`DecoderSessionBase.h`](../../../lib/audio/detail/DecoderSessionBase.h) and the concrete codec implementations own factory-only concrete construction, private initialization, and destruction of failed candidates.
- [`DecoderFactory.h`](../../../lib/audio/DecoderFactory.h) and [`DecoderFactory.cpp`](../../../lib/audio/DecoderFactory.cpp) own extension and MP4 sample-entry routing plus successful concrete-to-`DecoderSession` conversion.
- [`SegmentDecoderSession.cpp`](../../../lib/audio/SegmentDecoderSession.cpp) owns frame-exact range adaptation; [`TrackSession.cpp`](../../../lib/audio/detail/TrackSession.cpp) applies it to inspection and final decoding.

## Test map

Decoder session and malformed-input tests under [`test/audio/`](../../../test/audio) lock ready construction, requested-output adaptation, output representation, seeking, and failure behavior.
[`DecoderErrorTest.cpp`](../../../test/audio/DecoderErrorTest.cpp) mechanically rejects direct concrete construction and instance `close()` operations.
[`DecoderFactoryTest.cpp`](../../../test/audio/DecoderFactoryTest.cpp) locks ready construction, routing, and initialization failure.
[`DecoderOutputAdapterTest.cpp`](../../../test/audio/detail/DecoderOutputAdapterTest.cpp) directly locks rejection of precision loss, pass-through behavior, exact byte conversion, and reset.
[`WavDecoderChannelLayoutTest.cpp`](../../../test/audio/WavDecoderChannelLayoutTest.cpp) checks effective WAV identity, interpretation origin, refusal precedence, output adaptation, and PCM preservation across reads and seeks through EOF.
[`Mp3DecoderChannelLayoutTest.cpp`](../../../test/audio/Mp3DecoderChannelLayoutTest.cpp) is the channel-layout test authority for uniform mono and stereo/joint identity, dual and mixed-role withholding, same-count stereo/dual unknown admission without promotion, factory parity, exact zero PCM extent, output adaptation, seek/flush/EOF behavior, and refusal of geometry changes before contradictory PCM.
[`Mp3DecoderTrailingTagTest.cpp`](../../../test/audio/Mp3DecoderTrailingTagTest.cpp) covers supported APEv2/Lyrics3v2/ID3v1 suffixes, read-only descriptor agreement, APEv1 identity withholding, malformed and overlapping candidates, and PCM/seek/EOF preservation after input-boundary selection.
It also guards ID3v1-only scan reuse against a leading-junk lookahead failure and keeps a same-sized APE-only suffix on the rescan path.
The initial frame-information failure and frame/native geometry-disagreement checks remain defensive foreign-API guards, not independently driven fixture branches: the pinned mpg123 implementation shares track initialization between `getformat2` and `info2`, and the production signed-16 configuration requests native rates and channel counts without forced geometry conversion.
Foreign native conversion controls are not production-path coverage; no test-only production hook is used to manufacture those failures.
The encoded MP3 cases in [`Mp3DecoderSessionTest.cpp`](../../../test/audio/Mp3DecoderSessionTest.cpp) retain native gapless-trimmed totals, VBR scan duration, and exact seek positions, including native in-frame seek skips.
[`AlacDecoderChannelLayoutTest.cpp`](../../../test/audio/AlacDecoderChannelLayoutTest.cpp) covers ALAC ready-session identity, concrete/factory parity, literal PCM normalization, adaptation, seek-to-zero/flush, and EOF.
[`AacDecoderChannelLayoutTest.cpp`](../../../test/audio/AacDecoderChannelLayoutTest.cpp) covers final actual-output identity before the first public read, concrete/factory parity, unchanged literal FDK reference windows with a one-native-signed-16-LSB cross-implementation bound at their measured delay-relative phase, exact same-host native PCM and encoding adaptation, all supported encodings, unknown eight-channel output, and independent raw-native packet comparisons for cached first blocks, contiguous output, flush, and post-seek PCM at the requested frame before and after active decoding, plus stable EOF. It also checks packed-24 mid-packet suffixes, PCM continuity when DRC appears after a nonzero LC restart, and, with the Linux HE-AAC provider, single-rate SBR output against uninterrupted native PCM.
[`AacDecoderMalformedMp4Test.cpp`](../../../test/audio/AacDecoderMalformedMp4Test.cpp) distinguishes rejected zero-sample tables from admitted samples with empty payloads, and covers first-payload failures at open, required-error/request-preflight precedence, and later-payload read failures. Its Linux HE-AAC case checks that a seek-time decode error preserves the previous read position.
Linux-only [`AacAlsaChannelLayoutTest.cpp`](../../../test/audio/backend/AacAlsaChannelLayoutTest.cpp) uses real mono AAC PCM and controlled ALSA observations to check FC, ambiguous or missing positions, confirmed conflicts, and a zero-mask format-value control without opening hardware.
[`DemuxerAlacLayoutTest.cpp`](../../../test/media/mp4/DemuxerAlacLayoutTest.cpp) covers borrowed-entry association, extradata selection, and required container failures.
[`AlacChannelMetadataTest.cpp`](../../../test/audio/AlacChannelMetadataTest.cpp) covers the ALAC metadata matrix, and [`EngineAlacChannelLayoutTest.cpp`](../../../test/audio/EngineAlacChannelLayoutTest.cpp) covers Engine transitions.
[`AlacDecoderMalformedMp4Test.cpp`](../../../test/audio/AlacDecoderMalformedMp4Test.cpp) distinguishes malformed configuration at open from invalid sample data at read.
[`EngineWavChannelLayoutTest.cpp`](../../../test/audio/EngineWavChannelLayoutTest.cpp) locks real-WAV mode reuse versus drain/reopen, successor evidence publication, recoverable layout refusal before backend open, and rejected `setNext()` preservation of current playback while clearing old lookahead.
[`FlacDecoderChannelLayoutTest.cpp`](../../../test/audio/FlacDecoderChannelLayoutTest.cpp) checks FLAC default and declared identities, exact field-name matching, strict value parsing and duplicate resolution, unknown admission for invalid declarations, output adaptation, and PCM preservation across reads, seeks, and flush through EOF.
[`FlacMetadataInitializationTest.cpp`](../../../test/audio/FlacMetadataInitializationTest.cpp) covers initialization diagnostics for raw frames without STREAMINFO, including implicit rate/precision headers, with valid-STREAMINFO, prefix-tolerance, and truncated-metadata controls.
[`FlacCommentCompatibilityTest.cpp`](../../../test/audio/FlacCommentCompatibilityTest.cpp) protects bounded comment inspection, absorbing unknown identity after structural damage or invalid declarations, native-prefix first-block STREAMINFO anchoring, unknown identity for nonfirst STREAMINFO even with early valid declarations or fake padding markers, native outer-boundary failures, and large comment counts with literal PCM.
[`FlacWavMaskCompatibilityTest.cpp`](../../../test/audio/FlacWavMaskCompatibilityTest.cpp) compares WAV identity with retained reference-FLAC output bytes, pinning trimmed WAV identity versus unknown FLAC for excess masks and literal PCM preservation without a runtime encoder dependency.
[`EngineFlacChannelLayoutTest.cpp`](../../../test/audio/EngineFlacChannelLayoutTest.cpp) owns real WAV/FLAC transition coverage: matching default identities permit strict reuse for counts 1/2/3/4/7/8, whereas absent-mask five/six-channel FLAC differs from inferred WAV and drains/reopens.
It also covers successor evidence publication, different-role drain/reopen, recoverable malformed-outer-metadata failure before backend open, and rejected `setNext()` preservation of current playback while clearing old lookahead.
[`EngineFlacTransitionTest.cpp`](../../../test/audio/EngineFlacTransitionTest.cpp) covers six-channel real-FLAC producer transitions: equal known and equal unknown reuse controls, known↔unknown drain/reopen, different nonzero roles, captured PCM, and successor evidence publication.
[`FlacAlsaPipeWireRouteTest.cpp`](../../../test/audio/backend/FlacAlsaPipeWireRouteTest.cpp) drives real five/six-channel decoders through controlled ALSA and PipeWire parsed-map boundaries: untagged sources retain ordinal bytes across back/side observations, while explicit identities keep strict role checks and same-set permutations. It does not open hardware.
Linux-only [`OpusAlsaChannelLayoutTest.cpp`](../../../test/audio/backend/OpusAlsaChannelLayoutTest.cpp) uses real mono and six-channel Opus decodes with controlled ALSA parsed observations. It checks role-conflict refusals, same-set private permutations, and eligibility with missing or compatible partial observations.
For the complete role-conflict comparisons, the fixture sets only the copied format value's speaker mask to zero; the same observations then permit transport without permutation. No alternative decoder is involved.
Mono `MONO` observations remain unassigned rather than being relabelled FC, and accepted mono maps retain one-channel PCM bytes. These tests do not open hardware or establish stereo-only-device support.
[`OpusDecoderChannelLayoutTest.cpp`](../../../test/audio/OpusDecoderChannelLayoutTest.cpp) locks family 0/1/255 identity publication with literal per-count masks and exact per-slot PCM comparisons against family-255 ordinal decodes using independently declared RFC speaker roles.
It covers silent and repeated mapping patterns, including distinguishable seven/eight-channel controls, six-channel family-255 ordinal order, concrete/factory parity and unsupported-family refusal, adaptation, and immutable stream information through reads/seek/flush/EOF.
The generic borrowed-block byte-validity case across seek/flush until the next read is covered by [`OpusDecoderSessionTest.cpp`](../../../test/audio/OpusDecoderSessionTest.cpp), independent of channel-role identification.
[`VorbisCommentTest.cpp`](../../../test/media/VorbisCommentTest.cpp) protects the shared raw grammar, borrowed order and bytes, malformed prefixes/tails, visitor abort and exception behavior, large counts, and atomic import collection.
[`EngineStereoReuseTest.cpp`](../../../test/audio/EngineStereoReuseTest.cpp) covers WAV/ALAC stereo aliasing and FLAC/ALAC stereo opt-in splicing versus strict draining in both directions, with literal PCM and source-identity publication. An unsupported ALAC `chan` declaration supplies the zero-mask stereo fixture.
These controlled Engine cases do not certify native routing or audible gaplessness.
[`SegmentDecoderSessionTest.cpp`](../../../test/audio/SegmentDecoderSessionTest.cpp) covers range PCM, exact and millisecond seeks, stable EOF, early EOF, structural mismatches, and both decoder openings.
[`CarrierPlaybackTest.cpp`](../../../test/runtime/CarrierPlaybackTest.cpp) verifies the stored second segment through final runtime PCM and a nonempty zero-millisecond segment.

## Related documents

- [Playback architecture](README.md)
- [Supported audio files](../../reference/media/audio-file.md)
- [Track model](../../reference/library/model/track.md)
- [Decoder error reference](decoder-error.md)
