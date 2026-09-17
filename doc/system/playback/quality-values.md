---
id: playback.audio-quality-surface
---
# Audio quality values and presentation

Quality values carry evidence about source fidelity, changes along the output path, and confidence in that evidence.
This page defines their interpretation and shared presentation policy; [quality analysis](quality-analysis.md) defines how the analyzer derives them, and [quality ownership](quality.md) locates the responsible layers.

These are in-process values, not a persisted enum or wire protocol.
[Core declarations](../../../include/ao/audio/QualityAnalyzer.h), the [runtime state](../../../app/include/ao/rt/PlaybackState.h), and [UIModel declarations](../../../app/include/ao/uimodel/playback/quality/AudioQualityFormatter.h) own field types, defaults, and callable signatures.
Frontend adapters consume shared categories rather than deriving another quality rating.

## Quality levels

`worseQuality` uses the following rank; higher rank wins.
`Unknown` means absence of a useful rating, not the worst degradation. Confidence remains separate from severity, and neither a default confidence flag nor a lossless source proves a verified output path.

| Rank | Quality | Meaning | Default presentation category |
|---:|---|---|---|
| 0 | `Unknown` | No useful quality rating | `Unknown` |
| 1 | `BitwisePerfect` | No reported signal change | `Medal` |
| 2 | `LosslessPadded` | Bit-transparent integer widening | `Positive` |
| 3 | `LosslessFloat` | Bit-transparent float mapping or proven round trip | `Positive` |
| 4 | `LinearIntervention` | Reported conversion, gain, mute, or mixing | `Diagnostic` |
| 5 | `LossySource` | Encoded source is lossy | `Informational` |
| 6 | `Clipped` | Reserved sample-clipping severity | `Clipped` |

The current analyzer does not infer actual clipping from amplification metadata.
Software amplification remains `LinearIntervention`, with a `Warning` presentation override for clipping risk.
Consumers use the analyzer-assigned finding quality, not enum declaration order or a second finding-kind classifier.

## Finding kinds

This table describes semantic evidence, not display strings or struct layouts.

| Finding kind | Analyzer quality | Evidence and interpretation |
|---|---|---|
| `Unknown` | `Unknown` | No useful finding |
| `BitPerfect` | `BitwisePerfect` | No other finding at the assessed node |
| `LossySource` | `LossySource` | Source encoding is lossy |
| `SoftwareVolumeModification` | `LinearIntervention` | Reported software attenuation; positive gain can be displayed |
| `SoftwareAmplification` | `LinearIntervention` | Reported software gain above unity; a risk, not measured clipping |
| `HardwareVolumeModification` | `BitwisePerfect` | Gain attributed to hardware, not a digital-path modification |
| `UnclassifiedVolumeModification` | `LinearIntervention` | Gain change whose origin is unverified |
| `Muted` | `LinearIntervention` | Reported muting |
| `Resampling` | `LinearIntervention` | Source and destination sample rates |
| `ChannelMapping` | `LinearIntervention` | Source and destination channel counts/layout evidence |
| `LosslessPadding` | `LosslessPadded` | Integer precision fits the wider representation |
| `LosslessFloat` | `LosslessFloat` | Proven lossless float mapping |
| `LosslessRoundTrip` | `LosslessFloat` or `LosslessPadded` | Proven source precision survives a representation round trip |
| `Truncation` | `LinearIntervention` | Source/destination precision and sample domains |
| `MixedSources` | `LinearIntervention` | Additional sources; application names may be unavailable |

The analyzer's [node-property rules](quality-analysis.md#node-properties), [format transitions](quality-analysis.md#format-transitions), and [round-trip proof](quality-analysis.md#round-trip-proof) define how these findings arise, including gain thresholds and proof invalidation. [Result axes and confidence](quality-analysis.md#result-axes-and-confidence) defines the independent verification flag.

## Localized labels and numeric evidence

`AudioQualityFormatter` owns shared message selection through its catalog.
[Authored catalogs](../../../app/i18n/catalog/) and the [message inventory](../../../app/include/ao/i18n/MessageInventory.def) own exact text and argument signatures; there is no fixed-English output contract here.
See [text semantics](../presentation/text-catalog.md) for the domain mapping and locale-neutral formatting boundary.

- `Unknown` raw conclusions and `Unknown`/`BitPerfect` finding labels are empty. Hardware-volume findings have a label but are hidden from the structured headline's visible-finding set.
- Positive finite gain is shown as signed decibels with one fractional digit; non-positive or non-finite gain omits the number.
- Resampling and channel findings display both endpoints when available; missing endpoints select the catalog's generic form.
- Truncation wording distinguishes float-to-integer, integer-to-float, and same-domain precision loss. Precision evidence uses logical bits with `b` for integer and `f` for float.
- Format labels receive sample rate in kHz with one fractional digit, a bit count, and localized channel text. A signal format shows logical precision; a PCM node shows container width. Thus a 16-bit signal carried in `Signed32Le` displays 32 bits at that PCM node.
- Mono, stereo, other channel counts, node-role labels, sentence order, punctuation, and units in authored copy are catalog-owned. External application names remain data and are joined for the message argument.

## Structured headline precedence

The first matching row wins. Headline meanings below are not literal catalog strings.
The headline does not repeat per-node rates, gain, shared applications, or other details already carried by findings.

| Priority | Condition | Meaning | Category |
|---:|---|---|---|
| 1 | Overall quality is unknown | No usable pipeline rating | `Unknown` |
| 2 | Any software-amplification finding | Clipping risk | `Warning` |
| 3 | Any mute, truncation, resampling, software or unclassified volume modification, mixing, or channel mapping | Pipeline intervention | `Diagnostic` |
| 4 | Path is not fully verified | Incomplete path evidence | `Informational` |
| 5 | Any lossless padding, round trip, or float mapping | Signal preserved | `Positive` |
| 6 | Source is lossy | Clean delivery of a lossy source | `Informational` |
| 7 | No visible findings and source quality is bit-perfect | Bit-perfect playback | `Medal` |
| 8 | Otherwise | Clean delivery | `Positive` |

`BitPerfect` and `HardwareVolumeModification` do not count as visible findings here.
The structured headline has no direct `Clipped` branch; raw conclusion/category mapping and Soul aura do.
Do not equate a headline, raw severity, and confidence flag without applying their respective rules.

## Visual categories

GTK exposes these category hooks; native adapters own their styling mechanics.
Shared palette values and usage guidance live with the [brand recipe](../../../asset/brand/Soul.md#color-tokens), not in another color table here.

| Category | GTK CSS class |
|---|---|
| `Unknown` | none |
| `Medal` | `ao-quality-medal` |
| `Positive` | `ao-quality-positive` |
| `Diagnostic` | `ao-quality-diagnostic` |
| `Warning` | `ao-quality-warning` |
| `Informational` | `ao-quality-informational` |
| `Clipped` | `ao-quality-clipped` |

## Soul aura and motion

UIModel owns the `Animating`, `Frozen`, and `Dormant` motion policy, accumulated active elapsed time, motion sampling, and aura-plus-motion visual-frame composition. Soul maintains sampled motion separately from the current quality color.
Playing advances active elapsed time; paused playback freezes the exact sampled breath, rotation, luminance, and hue phase. Idle, opening, buffering, seeking, stopping, and error states are dormant and reset elapsed time and the sample.
While frozen, new readiness or quality can recolor the frame without changing its motion. Resume continues from the retained phase.

For Playing and Paused, aura selection uses the first matching rule:

| Priority | Condition | Aura |
|---:|---|---|
| 1 | Output not ready | `Veiled` |
| 2 | Overall or pipeline quality is clipped | `Burning` |
| 3 | Pipeline quality is linear intervention | `Turbulent` |
| 4 | Incomplete verification, or source quality is lossy/unknown | `Veiled` |
| 5 | Pipeline quality is bit-perfect | `Radiant` |
| 6 | Pipeline quality is lossless padded/float | `Flowing` |
| 7 | Pipeline quality is lossy/unknown | `Veiled` |

Other transport states select `Dormant` regardless of retained quality evidence.
Frontend adapters provide frame deltas and own clocks, frame registration, visibility/minimization gates, concrete geometry, and rendering. They may quantize the shared visual frame for their medium without changing aura or freeze semantics.

## Compatibility and evidence

No persisted numeric or source-compatibility promise applies to these C++ values.
Changes to severity, confidence, headline precedence, or shared motion/aura behavior require corresponding contract and test updates; ordinary translated copy remains catalog-owned.

- [`Quality.h`](../../../include/ao/audio/Quality.h), [`QualityAnalyzer.cpp`](../../../lib/audio/QualityAnalyzer.cpp), and [`Graph.h`](../../../include/ao/audio/flow/Graph.h) own ranks and evidence analysis.
- [`AudioQualityFormatter.cpp`](../../../app/uimodel/playback/quality/AudioQualityFormatter.cpp) owns message selection, categories, and headline precedence; [`AudioQualityCss.cpp`](../../../app/linux-gtk/playback/AudioQualityCss.cpp) adapts GTK classes.
- [`AobusSoulViewModel.cpp`](../../../app/uimodel/playback/soul/AobusSoulViewModel.cpp) owns shared motion and aura state.
- [`QualityAnalyzerTest.cpp`](../../../test/unit/audio/QualityAnalyzerTest.cpp), [`AudioQualityFormatterTest.cpp`](../../../test/unit/uimodel/playback/quality/AudioQualityFormatterTest.cpp), and [`AobusSoulViewModelTest.cpp`](../../../test/unit/uimodel/playback/soul/AobusSoulViewModelTest.cpp) cover analysis and shared policy.
- [`AobusSoulTest.cpp`](../../../test/unit/linux-gtk/app/AobusSoulTest.cpp), [`AudioPipelinePanelTest.cpp`](../../../test/unit/linux-gtk/playback/AudioPipelinePanelTest.cpp), and [`PlaybackPanelTest.cpp`](../../../test/unit/tui/PlaybackPanelTest.cpp) cover frontend consumption and frozen-frame behavior.
