# Audio Quality Analysis Model

Aobus uses a structured, per-node model for analyzing audio quality throughout the playback pipeline. This enables rich diagnostic UI rendering (e.g., node-specific coloring or detailed popovers) while decoupling the core audio library from presentation string formatting.

## Architecture

The core of the quality analysis model is a pure function:

```cpp
QualityResult analyzeAudioQuality(flow::Graph const& graph);
```

This function processes the `flow::Graph` of the current playback session and
evaluates the signal path from the source node (`ao-source`) to the final output
device.

### QualityResult

The resulting `QualityResult` contains:
1. `sourceQuality`: the source-file axis (`Unknown`, `BitwisePerfect`, or
   `LossySource`).
2. `pipelineQuality`: the worst non-source finding on the delivery pipeline.
3. `overall`: the worse of `sourceQuality` and `pipelineQuality`, retained for
   sorting, compatibility, and simple color mapping.
4. `fullyVerified`: a confidence flag. It is false when the analyzed playback
   path does not terminate at an output `Sink`, or when any node on that path
   did not report a format, because unreported downstream conversions cannot be
   confirmed or ruled out.
5. `assessments`: a sequence of `NodeQualityAssessment` objects, ordered by the
   signal path.

User-facing UI does not use `overall` as its headline. It renders the playback
delivery status from `sourceQuality`, `pipelineQuality`, `fullyVerified`, and
the concrete findings.

### NodeQualityAssessment

Each node in the audio path (Decoder, Engine, Stream, Sink, etc.) receives exactly one `NodeQualityAssessment`. An assessment contains:
- The node's identity (`nodeId`, `nodeName`, `nodeType`).
- The node's reported format (`optFormat`), when known. UI panels render the
  path directly from the ordered assessments instead of recomputing the path
  from the graph. Nodes without a reported format remain in the path and make
  the result partially verified.
- A `worstQuality` score, representing the most severe degradation that occurred at or immediately after this node.
- A list of `QualityFinding` entries detailing specific events.

### Quality Findings

Findings describe specific properties or transitions that impact audio quality.
They are categorized by `QualityFindingKind` and carry their analyzer-assigned
`quality` severity. UI code uses that severity directly; the mapping from
finding kind to severity has one owner in the analyzer.

**Node Self-Properties:**
- `LossySource`: The node decodes a lossy format (e.g., MP3, AAC).
- `SoftwareVolumeModification`: The node applies non-unity digital gain at or
  below unity (software volume attenuation), treated as a
  `LinearIntervention`. When the backend reports a magnitude, the finding
  carries that gain.
- `SoftwareAmplification`: The node applies software gain above unity, treated
  as a `LinearIntervention` and surfaced as a clipping-risk warning. The
  finding carries the maximum reported software gain.
- `HardwareVolumeModification`: The node applies non-unity volume via hardware controls. This is a neutral finding and does not downgrade the pipeline rating because it occurs after the digital bit-perfect path. A node with only hardware volume control is considered bit-perfect (maintains `BitwisePerfect` rating), and UI presentation does not let this neutral detail block the bit-perfect medal headline.
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
- `LosslessRoundTrip`: Proven integer-to-float-to-integer round trip where the
  original effective precision fits in the destination integer format and no
  precision-invalidating intervention occurred between the source and the
  float-to-integer hop.
- `Truncation`: Lossy precision reduction (e.g., 24-bit to 16-bit).

All transition findings include the full `optFromFormat` and `optToFormat` context.
Precision comparisons use `validBits` when a format reports it, falling back to
the container `bitDepth` otherwise. A valid-bits-only transition is reported
only when both sides report known valid-bit counts; an unknown valid-bit count
does not by itself prove padding or truncation. Channel count and precision are
assessed independently, so one hop can carry both `ChannelMapping` and
`Truncation` findings.

User-facing precision labels also use effective precision instead of container
width. Domain changes are described as quantization, for example `Float →
integer quantization: 32f → 24b`, while same-domain precision loss is shown as
`Precision truncated: 32b → 24b`.

**Round-trip proof:**
While walking the path, the analyzer tracks proven integer-source precision.
The proof starts only from a non-float source node's effective bits and is
invalidated by any unknown format or by an intervention finding such as
software volume, software amplification, unclassified volume, mute, resampling,
channel mapping, truncation, or external mixing. A
float-to-integer transition whose destination effective bits can represent the
proven source precision is classified as `LosslessRoundTrip` at
`LosslessFloat` severity. Without that proof, the same float-to-integer
transition remains `Truncation`. Float sources never receive this proof, because
writing arbitrary float samples to integer PCM is quantization.

**Bit-Perfect Paths:**
If a node has no other findings (no self-properties and no problematic transitions into it), the analyzer explicitly appends a `BitPerfect` finding. This ensures every node in a flawless path can be confirmed as bit-perfect by the UI, rather than leaving an empty list which might be misinterpreted as "untested".

## ALSA Software Fallback

When using the ALSA backend, Aobus employs a runtime probe to verify hardware mixer writability. If the probe fails, or if a hardware write fails during playback, the backend automatically transitions to software gain.

In software fallback mode:
- Volume is applied by multiplying PCM samples digitally.
- The `Sink` node in the graph is updated to report
  `SoftwareVolumeModification` if gain is non-unity and at or below unity.
- The sink carries `maxSoftwareGain` when the software-gain magnitude is known;
  gain above unity is reported as `SoftwareAmplification`.
- Quality analyzer correctly downgrades the pipeline to `LinearIntervention`.
- The UI reflects attenuation with a `Software volume attenuation` indicator in
  the pipeline view, and amplification with a `Software amplification` clipping
  risk warning.
- In the GTK frontend, the volume bar widget actively displays a "HW" badge in its top-left corner when the ALSA hardware mixer is functioning properly; this badge disappears if it falls back to software gain.

## UI Formatting

The `NowPlayingViewModel`, GTK pipeline panel, and TUI pipeline panel consume
the ordered `NodeQualityAssessment` data directly to generate user-friendly
tooltips and visual indicators. By having structured data, the UI layer can map
specific `QualityFindingKind` entries to localized strings and icons without
parsing text output from the core library or duplicating analyzer path logic.

The simple enum-label formatter reserves `Bit-perfect playback` for
`BitwisePerfect`. `LosslessPadded` and `LosslessFloat` are both rendered as
`Signal preserved` because both preserve decoded signal values without promising
byte-identical delivery.

The playback headline is a single-line, delivery-focused verdict. It does not
repeat rate changes, gain values, shared applications, or partial-verification
details that already belong to the per-node findings:

- `Bit-perfect playback`: lossless source, fully verified path, no findings.
- `Signal preserved`: padding-only delivery, bit-transparent float mapping, or
  proven float round trip.
- `Clean lossy delivery`: lossy source with no extra playback changes, presented as
  informational rather than positive.
- `Clean delivery`: otherwise clean delivery when no more specific verdict
  applies.
- `Pipeline intervention`: diagnostic pipeline findings such as resampling,
  software attenuation, unclassified volume changes, mixed sources, muting,
  channel mapping, or truncation.
- `Clipping risk`: warning headline for software amplification above unity.
- `Partially verified path`: an otherwise clean path with incomplete
  verification due to a missing output endpoint or missing path-node format
  reports.
- `Unknown pipeline`: no useful quality state is available yet.

Visual indicators use tier names rather than enum names: medal (bit-perfect
only), positive (preserved signal), diagnostic, warning,
informational, and clipped. Purple is reserved for the medal tier; green covers
positive delivery; orange (`#F59E0B`) covers diagnostic interventions and
amplification/clipping-risk warnings, matching the Soul turbulent aura; gray
covers informational state; red remains reserved for sample-level clipping
detection.
