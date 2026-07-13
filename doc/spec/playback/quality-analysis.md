---
id: playback.audio-quality
type: spec
status: current
domain: playback
summary: Defines playback-path quality analysis, evidence, confidence, runtime publication, and frontend-neutral verdict behavior.
---
# Audio quality analysis

## Scope

This specification defines how Aobus analyzes the active audio delivery path, separates source fidelity from pipeline intervention, proves or refuses bit-transparent conversions, publishes quality state through runtime, and derives frontend-neutral verdict categories.
The exact quality values, finding kinds, payload fields, labels, categories, and style tokens belong to the [audio quality surface reference](../../reference/playback/quality-surface.md).

Quality evidence ownership, graph composition, publication, and layer dependencies belong to the [audio quality architecture](../../architecture/audio-quality.md) within the broader [playback architecture](../../architecture/playback.md).
Decoder output guarantees belong to the [decoder session specification](decoder-session.md); this document begins with the formats and evidence reported by the assembled playback graph.

## Code boundary

This contract spans the Core audio, application runtime, and UIModel layers from the [system architecture](../../architecture/system-overview.md) under the ownership and dependency rules in the [audio quality architecture](../../architecture/audio-quality.md).
Its behavioral implementation is centered on [`QualityAnalyzer.cpp`](../../../lib/audio/QualityAnalyzer.cpp), runtime `QualityState`, and the shared UIModel quality formatter; frontends consume those values without redefining the behavior.

## Terminology

- **Playback graph** is the merged Core and platform-provider graph for the accepted route generation.
- **Playback path** is the active node chain beginning at `ao-source` and following active delivery connections.
- **Source axis** classifies whether the encoded source is known lossless or lossy.
- **Pipeline axis** classifies conversions and interventions after source identity.
- **Assessment** is the ordered quality record for one node on the playback path.
- **Finding** is one analyzer-owned observation attached to the node that owns or receives it.
- **Fully verified** means the analyzed path reaches a Sink and every path node reports a format.
- **Effective precision** is `validBits` when known, otherwise the format container bit depth.
- **Round-trip proof** is evidence that an integer source can pass through float and return to an integer representation without losing its proven source precision.

## Invariants

- `analyzeAudioQuality` is a pure function of one `flow::Graph` snapshot.
- Analysis begins at the node identified by `ao-source`; disconnected graph content is not treated as part of the playback path.
- Every node on the analyzed path produces exactly one ordered assessment, including nodes whose format is unknown.
- Source lossiness affects only the source axis; every other finding affects only the pipeline axis.
- `overall` is the worse analyzer severity across the two axes and is not the sole input to the user-facing headline.
- A missing Sink or any missing path-node format clears full-verification confidence without fabricating a conversion finding.
- A conversion finding is attached to the destination node because that node receives the changed representation.
- Sample rate, channel count, and precision are evaluated independently, so one transition may produce multiple findings.
- The analyzer owns the finding-to-severity mapping; runtime, UIModel, GTK, and TUI consume that severity instead of redefining it.
- Software amplification is clipping-risk evidence, not proof that samples clipped.

## State model

An empty graph or a graph without `ao-source` produces Unknown axes and no assessments.
A path that exists but does not reach a Sink is analyzed and remains partially verified.

### Result axes and confidence

When the playback path begins with a reported format, the source axis starts bitwise-perfect; an explicit lossy-source property changes only that axis to lossy.
The pipeline axis starts bitwise-perfect and accumulates every non-source finding by analyzer severity.
`overall` retains the worse of source and pipeline for compatibility, sorting, simple styling, and consumers that cannot show both axes.

`fullyVerified` is independent confidence evidence.
It is false when the path terminates before a Sink or any path node omits its format.
Missing evidence does not by itself downgrade the pipeline axis because Aobus cannot claim that an unreported conversion occurred.

Every assessment records node identity, type, name, optional format, its worst local severity, and ordered findings.
A node with no property or incoming-transition finding receives an explicit BitPerfect finding; a missing format still makes the whole result partially verified.

## Commands and transitions

### Node properties

The analyzer derives self-property findings from graph evidence:

- a lossy source marks the source assessment and source axis;
- non-unity software attenuation is a linear intervention and carries reported gain when available;
- software gain above unity is a linear intervention with a distinct amplification finding used for clipping-risk presentation;
- non-unity hardware volume is neutral to the digital-path rating;
- non-unity volume whose hardware/software location is unknown is conservatively a linear intervention;
- mute is a linear intervention;
- an external active input into a path node is mixed-source intervention, with sorted unique application names when the provider supplies them.

Hardware volume does not block a bit-perfect headline when it is the only non-BitPerfect detail.
The runtime volume snapshot independently exposes whether control is hardware-assisted; current shared volume presentation annotates that state in its tooltip rather than making a quality finding responsible for volume-control UI.

### Format transitions

For each adjacent pair with reported formats, the analyzer compares:

- sample rate, producing Resampling when it changes;
- channel count, producing ChannelMapping when it changes;
- precision domain and effective bit count, producing lossless padding, lossless float mapping, proven lossless round trip, or truncation.

Integer widening within the integer domain is lossless when destination effective precision is not smaller.
Float widening within the float domain is lossless float mapping when destination effective precision is not smaller; narrowing is truncation.
Integer-to-32-bit-float mapping is lossless only through 24 effective source bits; integer-to-64-bit-float mapping is lossless through 32 effective source bits.
Other integer-to-float changes are quantizing truncation.
Float-to-integer changes are truncation unless round-trip proof remains valid and the destination effective precision can contain the proven source precision.

A valid-bits-only precision change is classified only when both formats report nonzero valid-bit counts.
If either side omits valid bits and container width/domain does not otherwise change, the analyzer does not infer padding or truncation.

### Round-trip proof

Proof starts only from a non-float source with a reported format and records its effective precision.
It survives bit-transparent padding and float mapping.
It is invalidated by a missing format or any software/unclassified volume change, amplification, mute, resampling, channel mapping, truncation, or external mixing.

A float source never establishes integer round-trip proof.
Writing arbitrary float source samples to integer PCM therefore remains truncation even when the destination container is wide.

### ALSA volume fallback

The ALSA exclusive backend probes candidate playback mixer elements with write/readback verification before classifying volume as hardware-assisted.
If mixer setup finds no effective writable element, or a later hardware volume/mute write fails, the backend switches to software gain.
Its graph publication then marks non-unity gain as software rather than hardware, and Player re-runs quality analysis.

Software fallback applies gain in the PCM path and reports its current gain magnitude.
The analyzer classifies attenuation as software-volume intervention; any provider-reported gain above unity uses the amplification finding.
The application volume state simultaneously changes its hardware-assisted observation, so frontend volume presentation no longer claims hardware control.

## Failure and cancellation

Unknown or incomplete provider evidence is not an analyzer failure.
It produces Unknown state when no playback path exists, or a partially verified analyzed path when nodes exist but endpoint/format evidence is incomplete.
The analyzer never invents sample rate, channel, precision, volume provenance, or external application names.

Route-generation rejection, callback marshalling, and publication lifetime belong to the [audio quality architecture](../../architecture/audio-quality.md).
Provider graph changes may publish intermediate results while a route settles; behaviorally, `QualityChanged.ready` distinguishes whether Player currently has a usable selected output, and every accepted event agrees with the refreshed runtime snapshot.

Software amplification remains `LinearIntervention` in analysis and becomes a Warning in UIModel.
`Quality::Clipped` is reserved for independently observed sample-level clipping; the current graph analyzer does not emit it merely from gain metadata.

## Persistence and versioning

Quality analysis is derived live state.
Graphs, assessments, findings, confidence, verdicts, and categories are not persisted in the playback session and are recomputed after route, track, or provider evidence changes.

Changing an enum value, finding payload, fixed label, or style token affects the exact C++/presentation surface documented in the [audio quality reference](../../reference/playback/quality-surface.md).
Changing classification or precedence is a behavioral change to this specification and requires analyzer plus UIModel tests.

## Frontend observations

`PlaybackState::quality` mirrors source axis, pipeline axis, overall severity, confidence, and ordered assessments.
`PlaybackService::QualityChanged` publishes that same state plus route readiness on the callback executor.
Consumers may observe more than one event during route settlement and must render the latest accepted snapshot rather than rely on an exact event count.

UIModel derives one delivery-focused headline and visual category with this precedence:

1. no usable quality state;
2. software amplification warning;
3. concrete pipeline intervention;
4. otherwise clean but partially verified path;
5. signal-preserving padding, float mapping, or proven round trip;
6. clean delivery of a lossy source;
7. fully verified bit-perfect delivery;
8. otherwise clean delivery.

The headline does not repeat per-node rates, gain, shared applications, or other details already carried by findings.
The architectural requirement that pipeline panels consume ordered assessments and share UIModel policy belongs to the [audio quality architecture](../../architecture/audio-quality.md); exact strings and category styles belong to the [reference](../../reference/playback/quality-surface.md).

## Implementation map

- [`QualityAnalyzer.h`](../../../include/ao/audio/QualityAnalyzer.h) defines result, assessment, and finding values.
- [`QualityAnalyzer.cpp`](../../../lib/audio/QualityAnalyzer.cpp) owns graph traversal, evidence classification, proof, and severity aggregation.
- [`Player.cpp`](../../../lib/audio/Player.cpp) builds the merged graph, gates route generations, and publishes accepted quality results.
- Backend graph adapters under [`lib/audio/backend/`](../../../lib/audio/backend/) provide platform route evidence.
- [`PlaybackState.h`](../../../app/include/ao/rt/PlaybackState.h) and [`PlaybackService.cpp`](../../../app/runtime/PlaybackService.cpp) own runtime snapshot and event publication.
- [`AudioQualityFormatter`](../../../app/include/ao/uimodel/playback/quality/AudioQualityFormatter.h) owns shared presentation derivation.

## Test map

- [`QualityAnalyzerTest.cpp`](../../../test/unit/audio/QualityAnalyzerTest.cpp) proves axes, findings, attribution, precision, verification, float conversion, and proof invalidation.
- [`PlayerTest.cpp`](../../../test/unit/audio/PlayerTest.cpp) proves merged-graph handling, generation gating, incomplete provider evidence, callback marshalling, and readiness.
- [`AlsaGraphRegistryTest.cpp`](../../../test/unit/audio/backend/detail/AlsaGraphRegistryTest.cpp) proves ALSA hardware/software/unclassified graph evidence and gain publication.
- [`PlaybackServiceOutputTest.cpp`](../../../test/unit/runtime/PlaybackServiceOutputTest.cpp) proves snapshot/event agreement and route-ready publication.
- [`AudioQualityFormatterTest.cpp`](../../../test/unit/uimodel/playback/quality/AudioQualityFormatterTest.cpp) proves label, category, precision, gain, and headline precedence.
- [`AudioPipelinePanelTest.cpp`](../../../test/unit/linux-gtk/playback/AudioPipelinePanelTest.cpp) and [`QualityIndicatorStyleTest.cpp`](../../../test/unit/tui/QualityIndicatorStyleTest.cpp) prove frontend consumption of shared presentation state.

## Related documents

- [Audio quality architecture](../../architecture/audio-quality.md)
- [Playback architecture](../../architecture/playback.md)
- [Audio quality surface reference](../../reference/playback/quality-surface.md)
- [Decoder session](decoder-session.md)
- [Audio execution and concurrency](audio-execution.md)
