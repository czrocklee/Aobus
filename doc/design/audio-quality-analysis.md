# Audio Quality Analysis Model

Aobus uses a structured, per-node model for analyzing audio quality throughout the playback pipeline. This enables rich diagnostic UI rendering (e.g., node-specific coloring or detailed popovers) while decoupling the core audio library from presentation string formatting.

## Architecture

The core of the quality analysis model is a pure function:

```cpp
QualityResult analyzeAudioQuality(flow::Graph const& graph);
```

This function processes the `flow::Graph` of the current playback session and evaluates the signal path from the decoder (`ao-decoder`) to the final output device.

### QualityResult

The resulting `QualityResult` contains:
1. `quality`: The overall (worst) quality category of the entire pipeline.
2. `assessments`: A sequence of `NodeQualityAssessment` objects, ordered by the signal path.

### NodeQualityAssessment

Each node in the audio path (Decoder, Engine, Stream, Sink, etc.) receives exactly one `NodeQualityAssessment`. An assessment contains:
- The node's identity (`nodeId`, `nodeName`, `nodeType`).
- A `worstQuality` score, representing the most severe degradation that occurred at or immediately after this node.
- A list of `QualityFinding` entries detailing specific events.

### Quality Findings

Findings describe specific properties or transitions that impact audio quality. They are categorized by `QualityFindingKind`.

**Node Self-Properties:**
- `LossySource`: The node decodes a lossy format (e.g., MP3, AAC).
- `SoftwareVolumeModification`: The node applies non-unity digital gain (software volume attenuation), treated as a `LinearIntervention`.
- `HardwareVolumeModification`: The node applies non-unity volume via hardware controls. This is a neutral finding and does not downgrade the pipeline rating because it occurs after the digital bit-perfect path. A node with only hardware volume control is considered bit-perfect (maintains `BitwisePerfect` rating), though it does not carry an explicit `BitPerfect` finding.
  - **ALSA Note:** Hardware mixer volume is neutral only after Aobus successfully probes a writable mixer element. If ALSA hardware mixer control is unavailable or ineffective, Aobus falls back to software gain.
- `UnclassifiedVolumeModification`: The node applies non-unity gain but PipeWire lacks sufficient evidence to classify it as hardware or software. Treated conservatively as a `LinearIntervention`.
- `Muted`: The node is muting the signal.
- `MixedSources`: The node receives input from multiple sources (e.g., shared with another application). This finding carries the names of the sharing applications.

**Format Transitions:**
Format transitions occur between nodes. For accurate UI attribution, **conversion findings are always attributed to the destination node.** For example, if the Decoder outputs 44.1kHz and the Engine expects 48kHz, the Engine is responsible for the resampling, so the `Resampling` finding is attached to the Engine's assessment.

Transition findings include:
- `Resampling`: Sample rate conversion.
- `ChannelMapping`: Channel count conversion.
- `LosslessPadding`: Bit-transparent expansion (e.g., 16-bit integer to 24-bit integer).
- `LosslessFloat`: Bit-transparent integer-to-float mapping (e.g., 24-bit to 32-bit float).
- `Truncation`: Lossy precision reduction (e.g., 24-bit to 16-bit).

All transition findings include the full `optFromFormat` and `optToFormat` context.

**Bit-Perfect Paths:**
If a node has no other findings (no self-properties and no problematic transitions into it), the analyzer explicitly appends a `BitPerfect` finding. This ensures every node in a flawless path can be confirmed as bit-perfect by the UI, rather than leaving an empty list which might be misinterpreted as "untested".

## ALSA Software Fallback

When using the ALSA backend, Aobus employs a runtime probe to verify hardware mixer writability. If the probe fails, or if a hardware write fails during playback, the backend automatically transitions to software gain.

In software fallback mode:
- Volume is attenuated by multiplying PCM samples digitally.
- The `Sink` node in the graph is updated to report `SoftwareVolumeModification` if gain is non-unity.
- Quality analyzer correctly downgrades the pipeline to `LinearIntervention`.
- The UI reflects this with a yellow `Software volume attenuation` indicator.

## UI Formatting

The `NowPlayingViewModel` consumes `NodeQualityAssessment` data to generate user-friendly tooltips and visual indicators. By having structured data, the UI layer can cleanly map specific `QualityFindingKind` entries to localized strings and icons without parsing text output from the core library.
