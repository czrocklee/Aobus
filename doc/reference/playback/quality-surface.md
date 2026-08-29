---
id: playback.audio-quality-surface
type: reference
status: current
domain: playback
summary: Enumerates audio quality levels, findings, result fields, labels, verdict categories, and frontend style tokens.
---
# Audio quality surface

## Scope and version

This reference enumerates the current in-process C++ and shared presentation surface for playback quality analysis.
The behavior that produces and interprets these values belongs to the [audio quality specification](../../spec/playback/quality-analysis.md).

This surface has no serialized or wire-protocol version.
Its compatibility authority is the public Core audio, runtime state, and UIModel headers plus the tests linked below.

## Code boundary

This surface spans the **Core audio**, **application runtime**, and **UIModel** layers in the [system architecture](../../architecture/system-overview.md), following the ownership described by the [audio quality architecture](../../architecture/audio-quality.md) and [playback architecture](../../architecture/playback.md).
Core types live in `include/ao/audio/`, the runtime mirror lives in `app/include/ao/rt/PlaybackState.h`, and shared presentation types live in `app/include/ao/uimodel/playback/quality/` and `app/include/ao/uimodel/playback/soul/`; GTK, TUI, and WinUI adapt categories without redefining them.

## Surface

### Quality levels

`worseQuality` uses the rank shown below; higher rank wins.
Unknown is absence of a useful rating rather than the worst degradation, so confidence remains a separate field.

| Rank | `Quality` | Meaning | `audioQualityConclusion` | Default category |
|---:|---|---|---|---|
| 0 | `Unknown` | No useful quality rating. | Empty string | `Unknown` |
| 1 | `BitwisePerfect` | No reported signal change. | `Bit-perfect playback` | `Medal` |
| 2 | `LosslessPadded` | Bit-transparent integer widening. | `Signal preserved` | `Positive` |
| 3 | `LosslessFloat` | Bit-transparent float mapping or proven round trip. | `Signal preserved` | `Positive` |
| 4 | `LinearIntervention` | Reported conversion, gain, mute, or mixing intervention. | `Pipeline intervention` | `Diagnostic` |
| 5 | `LossySource` | Encoded source is lossy. | `Lossy source` | `Informational` |
| 6 | `Clipped` | Reserved sample-clipping severity. | `Signal clipping detected` | `Clipped` |

The current analyzer does not produce `Clipped` from software-amplification metadata.
`SoftwareAmplification` remains `LinearIntervention` and receives a presentation-category override to `Warning`.

### Finding kinds

| `QualityFindingKind` | Analyzer quality | Payload used | Shared base label |
|---|---|---|---|
| `Unknown` | `Unknown` | None | Empty |
| `BitPerfect` | `BitwisePerfect` | None | Empty |
| `LossySource` | `LossySource` | None | `Lossy source` |
| `SoftwareVolumeModification` | `LinearIntervention` | `gain` when positive | `Software volume attenuation` |
| `SoftwareAmplification` | `LinearIntervention` | `gain` when positive | `Software amplification (clipping risk)` |
| `HardwareVolumeModification` | `BitwisePerfect` | None | `Hardware volume control` |
| `UnclassifiedVolumeModification` | `LinearIntervention` | `gain` when positive | `Device volume change (source unverified)` |
| `Muted` | `LinearIntervention` | None | `Muted` |
| `Resampling` | `LinearIntervention` | `optFromFormat`, `optToFormat` | `Resampling` |
| `ChannelMapping` | `LinearIntervention` | `optFromFormat`, `optToFormat` | `Channel mapping` |
| `LosslessPadding` | `LosslessPadded` | `optFromFormat`, `optToFormat` | `Bit-transparent integer padding` |
| `LosslessFloat` | `LosslessFloat` | `optFromFormat`, `optToFormat` | `Bit-transparent float mapping` |
| `LosslessRoundTrip` | `LosslessFloat` or `LosslessPadded` | `optFromFormat`, `optToFormat` | `Bit-transparent round trip (float engine)` |
| `Truncation` | `LinearIntervention` | `optFromFormat`, `optToFormat` | `Precision truncated` |
| `MixedSources` | `LinearIntervention` | `sharedApps` | `Mixed sources` |

Dynamic finding labels refine the base text:

| Evidence | Exact shape |
|---|---|
| Positive attenuation gain | `Software volume attenuation: {signed dB with one decimal}` |
| Positive amplification gain | `Software amplification: {signed dB with one decimal} gain (clipping risk)` |
| Positive unclassified volume gain | `Device volume: {signed dB with one decimal} (source unverified)` |
| Resampling formats | `Resampling: {from Hz}Hz → {to Hz}Hz` |
| Channel formats | `Channels: {from}ch → {to}ch` |
| Float to integer truncation | `Float → integer quantization: {from precision} → {to precision}` |
| Integer to float truncation | `Integer → float quantization: {from precision} → {to precision}` |
| Same-domain truncation | `Precision truncated: {from precision} → {to precision}` |
| Named mixed sources | `Mixed with {comma-separated application names}` |

Precision text uses logical `precisionBits` as `{bits}b` for integer and `{bits}f` for float.
Gain text is omitted when gain is non-positive or non-finite.

### Value fields

`QualityFinding` fields:

| Field | Type | Default |
|---|---|---|
| `kind` | `QualityFindingKind` | `Unknown` |
| `quality` | `Quality` | `Unknown` |
| `gain` | `float` | `0.0F` |
| `optFromFormat` | `optional<NodeFormat>` | empty |
| `optToFormat` | `optional<NodeFormat>` | empty |
| `sharedApps` | `vector<string>` | empty |

`NodeQualityAssessment` fields:

| Field | Type | Default |
|---|---|---|
| `nodeId` | `string` | empty |
| `nodeName` | `string` | empty |
| `nodeType` | `flow::NodeType` | `Intermediary` |
| `optFormat` | `optional<NodeFormat>` | empty |
| `worstQuality` | `Quality` | `BitwisePerfect` |
| `findings` | `vector<QualityFinding>` | empty |

`QualityResult` and runtime `QualityState` both carry `sourceQuality`, `pipelineQuality`, `overall`, `fullyVerified`, and ordered `assessments`.
Both values default all quality axes to `Unknown`, `fullyVerified` to true, and assessments to empty.

### Node and format labels

| `flow::NodeType` | Shared label |
|---|---|
| `Source` | `[Source]` |
| `Decoder` | `[Decoder]` |
| `Engine` | `[Engine]` |
| `Stream` | `[Stream]` |
| `Intermediary` | `[Filter]` |
| `Sink` | `[Device]` |
| `ExternalSource` | `[Other Source]` |

`audioFormatLabel` produces `{sample rate in kHz with one decimal} · {bits}-bit · {channels}`.
Channels are `Mono`, `Stereo`, or `{count} ch`.
A `SignalFormat` label displays logical precision; a `PcmFormat` label displays `encodingContainerBits`, so a 16-bit signal widened into `Signed32Le` is labelled 32-bit at that PCM node.

### Structured headline precedence

`audioQualityPresentation` returns the first matching row:

| Priority | Condition | Headline | Category |
|---:|---|---|---|
| 1 | `overall == Unknown` | `Unknown pipeline` | `Unknown` |
| 2 | Any `SoftwareAmplification` finding | `Clipping risk` | `Warning` |
| 3 | Any `Muted`, `Truncation`, `Resampling`, `SoftwareVolumeModification`, `UnclassifiedVolumeModification`, `MixedSources`, or `ChannelMapping` finding | `Pipeline intervention` | `Diagnostic` |
| 4 | `fullyVerified == false` | `Partially verified path` | `Informational` |
| 5 | Any `LosslessPadding`, `LosslessRoundTrip`, or `LosslessFloat` finding | `Signal preserved` | `Positive` |
| 6 | `sourceQuality == LossySource` | `Clean lossy delivery` | `Informational` |
| 7 | No visible findings and `sourceQuality == BitwisePerfect` | `Bit-perfect playback` | `Medal` |
| 8 | Otherwise | `Clean delivery` | `Positive` |

`BitPerfect` and `HardwareVolumeModification` are excluded from visible findings.
The structured headline function has no direct `Clipped` branch.
The raw conclusion/category and Soul aura surfaces have explicit `Clipped` mappings.

### Visual categories

| `AudioQualityCategory` | GTK CSS class | Brand color |
|---|---|---|
| `Unknown` | none | Veiled `#6B7280` in TUI/Soul fallbacks |
| `Medal` | `ao-quality-medal` | `#A855F7` |
| `Positive` | `ao-quality-positive` | `#10B981` |
| `Diagnostic` | `ao-quality-diagnostic` | `#F59E0B` |
| `Warning` | `ao-quality-warning` | `#F59E0B` |
| `Informational` | `ao-quality-informational` | `#6B7280` |
| `Clipped` | `ao-quality-clipped` | `#EF4444` |

### Soul aura and motion

`AobusSoulViewState` exposes a `SoulAura aura` and an `AobusSoulMotionMode motionMode`.
`AobusSoulVisualFrame` exposes an `AobusSoulMotionFrame motion` and `AobusSoulGradientColors gradientColors`.
`AobusSoulAnimationState` owns accumulated active elapsed time and the current motion sample.
`advance(delta)` changes that sample only in `Animating`; `Frozen` retains the exact breath, rotation, luminance, and hue-shift values; `Dormant` resets elapsed time and the sample.
`visualFrame(aura)` composes the retained sample with the current aura, so a quality update can recolor a frozen frame without changing its motion values.
`aobusSoulVisualAt(aura, elapsed)` remains the pure direct-sampling operation, while `aobusSoulVisualFrame(aura, motion)` composes an already sampled or frozen frame.
The transport mapping is:

| Transport | Aura input | `motionMode` |
|---|---|---|
| `Playing` | Current readiness and quality | `Animating` |
| `Paused` | Current readiness and quality | `Frozen` |
| `Idle`, `Opening`, `Buffering`, `Seeking`, `Stopping`, `Error` | `Dormant` | `Dormant` |

For Playing and Paused, `resolveSoulAura` selects the first matching row:

| Priority | Condition | `SoulAura` | Brand color |
|---:|---|---|---|
| 1 | Output is not ready | `Veiled` | `#6B7280` |
| 2 | Overall or pipeline quality is `Clipped` | `Burning` | `#EF4444` |
| 3 | Pipeline quality is `LinearIntervention` | `Turbulent` | `#F59E0B` |
| 4 | Path is not fully verified, or source quality is `LossySource` or `Unknown` | `Veiled` | `#6B7280` |
| 5 | Pipeline quality is `BitwisePerfect` | `Radiant` | `#A855F7` |
| 6 | Pipeline quality is `LosslessPadded` or `LosslessFloat` | `Flowing` | `#10B981` |
| 7 | Pipeline quality is `LossySource` or `Unknown` | `Veiled` | `#6B7280` |

Paused presentation stops frame advancement and preserves the last drawn motion sample.
Later accepted readiness and quality snapshots replace its aura and recompute the gradient around the frozen hue shift; they do not reset its angle, breath, luminance, or hue phase.
Resuming continues from the retained elapsed phase.
Frontend adapters own clocks, frame registration, and rendering mechanics but do not change the shared aura or motion-mode policy.

## Validation rules

- Software amplification requires reported maximum software gain greater than `1.0F + 1e-4F`.
- Software attenuation uses positive minimum gain when reported, otherwise maximum gain.
- Transition findings require both source and destination formats.
- A precision transition compares source or encoding precision/domain plus concrete PCM representation width; it never infers PCM layout from `SignalFormat::precisionBits`.
- Same-domain float widening is `LosslessFloat`; same-domain float narrowing is `Truncation`.
- Integer-to-32-bit-float is lossless through 24 logical bits.
- Integer narrowing is `LosslessRoundTrip` with `LosslessPadded` while the destination still holds the proven source precision, and `Truncation` once that proof is gone.
- Shared application names are sorted and deduplicated; an unnamed external source still produces `MixedSources` with an empty list.
- Every assessment with no other finding receives exactly one `BitPerfect` finding.
- `fullyVerified` is false when the analyzed path lacks a terminal Sink or any path assessment lacks a format.

## Compatibility and versioning

These are internal C++ and UI presentation values with no persisted numeric compatibility promise.
Changing enum membership, severity rank, field shape, fixed labels, headline order, CSS class, or shared brand color requires matching specification and test updates.

The analyzer-assigned `QualityFinding.quality` is the behavioral authority at runtime.
Consumers must not infer severity from enum declaration order or duplicate a finding-kind mapping.

## Implementation authority

- [`Quality.h`](../../../include/ao/audio/Quality.h) owns quality levels.
- [`QualityAnalyzer.h`](../../../include/ao/audio/QualityAnalyzer.h) owns findings, assessments, results, and analyzer entry point.
- [`Graph.h`](../../../include/ao/audio/flow/Graph.h) owns graph node and connection evidence fields.
- [`PlaybackState.h`](../../../app/include/ao/rt/PlaybackState.h) owns the runtime mirror.
- [`AudioQualityFormatter.h`](../../../app/include/ao/uimodel/playback/quality/AudioQualityFormatter.h) and [`AudioQualityFormatter.cpp`](../../../app/uimodel/playback/quality/AudioQualityFormatter.cpp) own shared labels, categories, and headline precedence.
- [`AudioQualityCss.cpp`](../../../app/linux-gtk/playback/AudioQualityCss.cpp) and [`_variables.css`](../../../app/linux-gtk/css/_variables.css) own GTK class/color adapters.
- [`AobusSoulViewModel.h`](../../../app/include/ao/uimodel/playback/soul/AobusSoulViewModel.h) and [`AobusSoulViewModel.cpp`](../../../app/uimodel/playback/soul/AobusSoulViewModel.cpp) own the shared Soul aura and motion surface.

## Test authority

- [`QualityAnalyzerTest.cpp`](../../../test/unit/audio/QualityAnalyzerTest.cpp) locks the finding and severity surface.
- [`AudioQualityFormatterTest.cpp`](../../../test/unit/uimodel/playback/quality/AudioQualityFormatterTest.cpp) locks labels, mappings, and structured headlines.
- [`AobusSoulViewModelTest.cpp`](../../../test/unit/uimodel/playback/soul/AobusSoulViewModelTest.cpp) locks transport-aware Soul aura and motion mappings.
- [`AobusSoulTest.cpp`](../../../test/unit/linux-gtk/app/AobusSoulTest.cpp) and [`PlaybackPanelTest.cpp`](../../../test/unit/tui/PlaybackPanelTest.cpp) lock exact-frame pause and live paused-aura adaptation.
- [`AudioPipelinePanelTest.cpp`](../../../test/unit/linux-gtk/playback/AudioPipelinePanelTest.cpp) locks GTK pipeline/category consumption.
- [`PlaybackPanelTest.cpp`](../../../test/unit/tui/PlaybackPanelTest.cpp) locks TUI category colors and raw conclusion labels.

## Related documents

- [Audio quality analysis specification](../../spec/playback/quality-analysis.md)
- [Audio quality architecture](../../architecture/audio-quality.md)
- [Playback architecture](../../architecture/playback.md)
- [PCM format surface](pcm-format.md)
- [Decoder session](../../spec/playback/decoder-session.md)
