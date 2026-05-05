# Playback Control-Plane Refactor Plan

Date: 2026-04-29

## Goal

Refactor Aobus playback so that:

- the data plane is responsible only for decoding, PCM transport, negotiated playback format, and transport control
- the control plane is responsible for device discovery, live topology observation, graph assembly, and audio-quality analysis

This document turns the proposed direction into a concrete implementation plan that can be executed incrementally without breaking playback or the status UI.

## Current Problem Summary

The current implementation mixes three different concerns inside `PlaybackEngine`:

- track opening and source/backend coordination
- semantic graph assembly
- audio-quality analysis

Today the flow looks like this:

1. `PlaybackController` owns backend managers and chooses a backend.
2. `PlaybackEngine` opens the decoder and backend.
3. The backend pushes graph updates through `AudioRenderCallbacks::onGraphChanged`.
4. `PlaybackEngine` merges the backend graph with the internal decoder/engine nodes.
5. `PlaybackEngine` runs the quality-analysis algorithm and stores the final graph and quality result in `PlaybackSnapshot`.

That has several drawbacks:

- `PlaybackEngine` owns non-realtime control logic that is unrelated to PCM transport.
- `IAudioBackend` is forced to expose graph-monitoring behavior in addition to audio rendering callbacks.
- PipeWire graph observation is duplicated conceptually between `PipeWireManager` and `PipeWireBackend`.
- PipeWire stream identification is fragile because the monitor can fall back to matching the stream node by name instead of an explicit runtime identifier.
- `PlaybackController` is too passive and does not yet act as the owner of playback topology and quality conclusions.

## Design Objective

Adopt a strict split between:

- data plane: `PlaybackEngine` plus `IAudioBackend`
- control plane: `PlaybackController` plus `IBackendManager`

The intended ownership after the refactor is:

- `PlaybackEngine`: decoder session, PCM source, backend open/start/stop/pause/resume/seek, internal route description
- `IAudioBackend`: PCM I/O and backend runtime errors only
- `IBackendManager`: device discovery, system graph observation, graph subscriptions
- `PlaybackController`: active manager tracking, internal/system graph merge, final `AudioGraph`, `AudioQuality`, tooltip synthesis, cached playback snapshot exposure to the UI

## Non-Goals

This refactor should not attempt to do the following:

- redesign the user-facing output selection UI
- redesign `AudioNode`, `AudioLink`, or `AudioQuality` semantics beyond what is needed to move ownership
- add new DSP stages or new audio-quality heuristics
- generalize the Linux playback stack for Windows in the same change
- rewrite the whole playback polling model in `MainWindow`

## Current Code Paths

Relevant code at the time of writing:

- `app/core/playback/PlaybackEngine.cpp`
- `app/core/playback/PlaybackEngine.h`
- `app/core/playback/PlaybackController.cpp`
- `app/core/playback/PlaybackController.h`
- `app/core/playback/PlaybackTypes.h`
- `app/core/backend/IAudioBackend.h`
- `app/core/backend/IBackendManager.h`
- `app/platform/linux/playback/PipeWireManager.cpp`
- `app/platform/linux/playback/PipeWireBackend.cpp`
- `app/platform/linux/playback/PipeWireInternal.cpp`
- `app/platform/linux/playback/AlsaManager.cpp`
- `app/platform/linux/playback/AlsaExclusiveBackend.cpp`
- `app/platform/linux/ui/StatusBar.cpp`

## Target Architecture

### Data Plane

The data plane owns only the objects required to get PCM from a file into the selected backend:

- decoder session
- PCM source
- negotiated engine output format
- backend open/start/stop/pause/resume/seek lifecycle
- backend underruns and terminal backend errors

The data plane should still expose enough metadata for the control plane to reason about quality, but it should not perform graph assembly or graph interpretation.

### Control Plane

The control plane owns the full semantic playback path visible to the UI:

- available backends and devices
- active backend manager
- active playback route subscription
- application-internal route snapshot from the engine
- system graph snapshot from the backend manager
- merged playback graph
- quality level and quality tooltip

## High-Level Flow After Refactor

1. `PlaybackController` creates and owns backend managers for the full app lifetime.
2. `PlaybackController` chooses an output device and records which manager owns it.
3. `PlaybackEngine` opens playback on a backend and publishes an internal route snapshot.
4. Once the backend stream is ready, the engine publishes a backend route anchor for the control plane.
5. `PlaybackController` subscribes to the active manager for a system graph rooted at that anchor.
6. The controller merges the internal route and the system graph into the final `AudioGraph`.
7. The controller runs the quality-analysis algorithm and writes the result into the cached playback snapshot returned to the UI.

## Core Design Decisions

### 1. Keep `PlaybackSnapshot` As The UI Contract

The status UI already consumes:

- `PlaybackSnapshot::graph`
- `PlaybackSnapshot::quality`
- `PlaybackSnapshot::qualityTooltip`

Those fields should remain in `PlaybackSnapshot` so the UI contract stays stable. The ownership of how those fields are produced should move from the engine to the controller.

### 2. Replace Backend Graph Push With Manager Graph Subscription

Graph observation should no longer be part of `AudioRenderCallbacks`.

Instead:

- `PlaybackEngine` provides an internal route snapshot and a backend route anchor
- `PlaybackController` subscribes to the active manager for system graph updates

This change removes graph concerns from the realtime backend callback surface.

### 3. Track The Active Manager Explicitly

`PlaybackController` must remember which manager created the current backend. That manager becomes the owner of:

- stream-anchor interpretation
- system graph subscription
- backend-specific route semantics

Without explicit active-manager tracking, the controller cannot safely ask for the graph of the current playback route.

### 4. Avoid A PipeWire-Specific Controller Interface

PipeWire uses a live runtime node ID, but the controller should not know that detail directly.

The controller should work with a backend-neutral route-anchor object. The manager interprets it.

### 5. Build For Incremental Migration

The old backend graph callback should not be removed until the manager-based graph subscription path is already producing the same UI-visible result.

## Proposed Data Model Changes

### Engine-Owned Route Description

Add a small engine-owned snapshot type that describes only the application-internal path.

Suggested shape:

```cpp
struct BackendRouteAnchor final
{
  backend::BackendKind backend = backend::BackendKind::None;
  std::string id;

  bool operator==(BackendRouteAnchor const&) const = default;
};

struct EngineRouteSnapshot final
{
  backend::AudioGraph graph;
  std::optional<BackendRouteAnchor> anchor;

  bool operator==(EngineRouteSnapshot const&) const = default;
};
```

Semantics:

- `graph` contains only Aobus-owned nodes and links
- at minimum that includes decoder -> engine
- `anchor` identifies the backend-side attachment point once playback has a stable route

The `id` field is intentionally opaque to the controller. For PipeWire it can hold the runtime stream node ID encoded as text. For ALSA it can hold a fixed route key.

### Manager-Owned System Graph Snapshot

Keep using `backend::AudioGraph` as the normalized graph representation returned to the controller.

The manager may still maintain an internal raw representation, but the controller should receive a stable normalized `AudioGraph` and not PipeWire-native structs.

### Subscription Handle

Add an explicit subscription object so graph subscriptions can be safely torn down on stop, seek-driven reopen, device switch, or track switch.

Suggested shape:

```cpp
class IGraphSubscription
{
public:
  virtual ~IGraphSubscription() = default;
};
```

Managers can implement the actual lifetime semantics behind that handle.

### Optional Snapshot Split In `PlaybackSnapshot`

The public `PlaybackSnapshot` can remain unchanged initially.

Internally, the controller should treat it as composed from:

- transport state from the engine
- device inventory from managers
- merged graph and quality fields from the controller

No UI-facing split is required in the first phase.

## Proposed Interface Changes

### `IBackendManager`

Extend `IBackendManager` from device factory to long-lived system-graph observer.

Suggested additions:

```cpp
class IGraphSubscription;

class IBackendManager
{
public:
  using OnDevicesChangedCallback = std::function<void()>;
  using OnGraphChangedCallback = std::function<void(AudioGraph const&)>;

  virtual ~IBackendManager() = default;

  virtual void setDevicesChangedCallback(OnDevicesChangedCallback callback) = 0;
  virtual std::vector<AudioDevice> enumerateDevices() = 0;
  virtual std::unique_ptr<IAudioBackend> createBackend(AudioDevice const& device) = 0;

  virtual std::unique_ptr<IGraphSubscription> subscribeGraph(std::string_view routeAnchor,
                                                             OnGraphChangedCallback callback) = 0;
};
```

Notes:

- `routeAnchor` is opaque to the controller even if the concrete manager treats it as a PipeWire node ID.
- The callback should deliver normalized `AudioGraph`, not implementation-specific graph objects.
- Returning a subscription object is preferred over a separate `unsubscribeGraph()` API because it reduces lifecycle mistakes.

### `IAudioBackend`

Remove graph reporting from `AudioRenderCallbacks` once the controller path is ready.

Target shape:

```cpp
struct AudioRenderCallbacks final
{
  void* userData = nullptr;
  std::size_t (*readPcm)(void* userData, std::span<std::byte> output) noexcept = nullptr;
  bool (*isSourceDrained)(void* userData) noexcept = nullptr;
  void (*onUnderrun)(void* userData) noexcept = nullptr;
  void (*onPositionAdvanced)(void* userData, std::uint32_t frames) noexcept = nullptr;
  void (*onDrainComplete)(void* userData) noexcept = nullptr;
  void (*onBackendError)(void* userData, std::string_view message) noexcept = nullptr;
};
```

This restores `IAudioBackend` to a transport-focused interface.

### `PlaybackEngine`

Add controller-facing route reporting.

Suggested additions:

```cpp
class PlaybackEngine final
{
public:
  using OnRouteChanged = std::function<void(EngineRouteSnapshot const&)>;

  void setOnRouteChanged(OnRouteChanged callback);
  EngineRouteSnapshot routeSnapshot() const;
};
```

Semantics:

- `routeSnapshot()` returns only the internal route plus the current backend route anchor if known
- `setOnRouteChanged()` allows the controller to refresh merged graph state without waiting for the next UI poll

The engine should emit route changes when:

- a new track opens successfully
- the negotiated engine output format changes
- the backend route anchor becomes available
- playback is reset to idle

### Backend Route Anchor Publication

The engine needs a way to learn the backend route anchor.

There are two viable options:

1. Add a lightweight backend callback such as `onRouteReady(void*, std::string_view anchor)`.
2. Add a backend getter such as `currentRouteAnchor()` and have the engine read it after open.

Preferred option:

- use an explicit callback because PipeWire stream node ID becomes valid asynchronously after stream creation and negotiation

Suggested callback:

```cpp
void (*onRouteReady)(void* userData, std::string_view routeAnchor) noexcept = nullptr;
```

This is still data-plane information because it identifies the backend-side attachment point of the active stream, not the full graph.

## Concrete Component Responsibilities

### `PlaybackEngine`

After the refactor, `PlaybackEngine` should:

- keep opening decoder sessions and PCM sources
- keep negotiating backend format
- keep producing `ao-decoder` and `ao-engine` nodes
- optionally keep position calculation based on the engine output format
- publish internal route snapshots and route anchors
- stop owning graph merge logic
- stop owning audio-quality analysis

`PlaybackEngine` should no longer:

- receive full backend graph snapshots
- bridge `ao-engine` to a backend stream node itself
- assign final `AudioQuality`
- synthesize the quality tooltip

### `PlaybackController`

After the refactor, `PlaybackController` should:

- keep device enumeration caching
- keep output switching
- track which manager is active for the current backend
- subscribe to system-graph updates for the active route
- cache the latest internal route snapshot from the engine
- cache the latest system graph from the active manager
- merge the final graph
- run audio-quality analysis
- write graph/quality fields into the snapshot returned to the UI

### `PipeWireManager`

After the refactor, `PipeWireManager` should:

- keep a single long-lived PipeWire connection and monitor for app lifetime
- enumerate sinks from that monitor
- accept graph subscriptions rooted at a specific route anchor
- reuse the existing registry mirror and graph traversal logic
- deliver normalized `AudioGraph` snapshots to the controller

### `PipeWireBackend`

After the refactor, `PipeWireBackend` should:

- create and own only playback runtime objects needed for PCM output
- report stream readiness through a route-anchor callback
- report negotiated playback format if needed by the engine
- stop owning a private graph-monitoring pipeline

This is a major simplification over the current state, where `PipeWireBackend` also owns a `PipeWireMonitor` instance.

### `AlsaManager`

After the refactor, `AlsaManager` should:

- keep current device enumeration behavior
- synthesize a stable static graph for the current route subscription

The ALSA system graph can remain:

- `alsa-stream` -> `alsa-sink`

with the selected device and negotiated format applied.

## PipeWire Implementation Plan

### Current Reusable Pieces

The existing `PipeWireMonitor` already provides most of the hard parts:

- registry tracking of nodes and links
- sink discovery
- sink format and property observation
- graph traversal from the stream node to reachable sinks and related nodes

The implementation should reuse that logic instead of replacing it.

### Required Refactor In `PipeWireMonitor`

`PipeWireMonitor` currently mixes three jobs:

- sink enumeration
- playback-stream tracking
- direct backend callback delivery

Refactor it into two conceptual layers:

1. registry mirror and graph builder
2. subscription dispatch

Suggested internal shape:

- registry state: nodes, links, sink formats, sink properties, capabilities
- graph-query API: build normalized graph for a supplied stream node anchor
- subscription table: route-anchor -> callback registrations

### Graph Query Behavior

The manager-side graph query should:

1. parse the route anchor into the concrete PipeWire stream node ID
2. traverse active links starting from that node
3. compute the reachable playback path
4. identify sink candidates and the preferred sink node
5. include relevant external sources when they affect a reachable sink for mixing analysis
6. normalize the graph into `backend::AudioGraph`

### Subscription Behavior

When the PipeWire registry changes:

- the manager recomputes only the active subscriptions that could be affected
- each subscription receives an updated normalized `AudioGraph`

Initial version simplification:

- it is acceptable to recompute all active subscriptions on each relevant refresh because Aobus normally has at most one active playback subscription

### Stream Route Anchor Format

PipeWire route anchor payload:

- decimal string representation of the runtime `pw_stream` node ID

Example:

- `"91"`

The controller treats this as opaque text. Only `PipeWireManager` interprets it.

### How `PipeWireBackend` Should Publish The Anchor

`PipeWireBackend` should report the route anchor once the stream node ID is stable.

Candidate points:

- immediately after `pw_stream_connect()` if the node ID is already available
- on stream state change when the stream transitions to a connected or paused/streaming state
- on the first refresh where `pw_stream_get_node_id()` returns a valid ID

Implementation requirement:

- publish only when the node ID changes from `PW_ID_ANY` to a valid value
- avoid repeated callback spam for the same anchor

## ALSA Implementation Plan

ALSA does not need dynamic topology tracking.

Manager-side behavior is sufficient:

- `subscribeGraph()` returns a subscription that immediately invokes the callback with a synthesized graph
- the graph is derived from the selected route anchor and the last negotiated backend format if available

Suggested ALSA route anchor format:

- the selected device ID, or a prefixed string such as `"alsa:<device-id>"`

The graph can remain simple:

- stream node representing the ALSA playback stream
- sink node representing the selected ALSA device
- one active link between them

If the negotiated backend format is not available yet, the manager can initially emit a graph without format metadata and update it once the engine publishes a new internal route snapshot that includes the backend-side expected format.

## Graph Merge Strategy In `PlaybackController`

### Inputs

The controller should merge:

- internal graph from the engine
- system graph from the active manager

### Merge Rule

The final graph is:

1. all internal graph nodes and links
2. all system graph nodes and links
3. one bridge link from `ao-engine` to the manager-reported stream node, if a stream node exists

### Required Normalization Rule

The manager must always expose exactly one stream node for the active route subscription.

Reason:

- the UI tooltip and current analysis algorithm assume there is a clear linear playback path starting at `ao-decoder`

If the manager cannot identify a stream node, the controller should keep the internal graph only and mark quality as `Unknown` or preserve a degraded intermediate result.

### Merge Cache

The controller should cache:

- latest engine route snapshot
- latest manager system graph
- merged graph
- quality result
- quality tooltip

It should only recompute when one of the inputs changes.

## Audio-Quality Analysis Migration

### What Moves

Move from `PlaybackEngine` into `PlaybackController`:

- path building from `ao-decoder`
- active-link traversal for the total-order path
- mixing detection using input-source sets
- node-state checks such as lossy source, mute, volume modification
- format-transition checks such as resampling, channel change, lossless padding, float mapping, truncation
- final quality conclusion and tooltip text generation

### What Stays In The Engine

The helper that decides whether a bit-depth change is lossless can either:

- move with the analysis code into the controller implementation file
- or be extracted into a small playback-analysis helper only if both sides truly need it

Preferred choice:

- move it with the controller analysis code and keep it private to the new owner

### Analysis Trigger Rules

Run analysis when any of the following changes:

- internal route graph
- route anchor
- system graph
- playback reset to idle

Do not run the analysis on every `snapshot()` poll if the inputs are unchanged.

### Idle-State Behavior

On idle or stop:

- clear the subscribed system graph
- clear the merged graph
- set quality to `Unknown`
- clear the quality tooltip

This preserves the current UI behavior while avoiding stale topology from the last track.

## Concurrency And Lifecycle Rules

### Main Rule

All controller-owned snapshot mutation should happen on the main thread or through the existing main-thread dispatcher.

### Why This Matters

Today the engine already dispatches backend-originated events back to the main thread before mutating UI-visible state. The control-plane refactor must preserve that safety property.

### Generation Tracking

Add a playback-generation counter in the controller.

Increment it when:

- a new track starts
- output device changes
- playback is stopped and reset

Each graph subscription callback should capture the generation it belongs to. If a callback arrives for an old generation, ignore it.

This prevents stale graph updates from a previous stream from overwriting current playback state.

### Subscription Lifetime

The controller should destroy the current graph subscription when:

- playback stops
- output switches
- playback backend is replaced
- a new route anchor supersedes the old one

Destroying the subscription must be enough to stop future graph callbacks for that route.

### Locking Guidance

- keep engine state locking focused on transport state
- keep controller graph state under its own mutex only if cross-thread access cannot be avoided
- if graph callbacks are always dispatched onto the main thread, prefer avoiding extra controller locking where possible

## Incremental Migration Plan

### Phase 1: Prepare The Controller To Own Graph State

Changes:

- add controller-owned cached merged graph and quality fields
- add active-manager tracking
- add engine route callback or polling-based route snapshot seam
- keep the existing engine graph/quality path in place for now

Expected result:

- no user-visible behavior change
- controller can start observing the same data it will later own

### Phase 2: Add Manager Graph Subscription API

Changes:

- extend `IBackendManager` with `subscribeGraph()`
- implement a no-op or trivial static version in `NullManager`
- implement a static synthesized graph subscription in `AlsaManager`
- implement manager-owned graph subscription in `PipeWireManager`

Expected result:

- managers can provide normalized system graphs independent of `IAudioBackend`

### Phase 3: Publish Route Anchors From The Data Plane

Changes:

- add `onRouteReady` or equivalent route-anchor reporting from backends to the engine
- add engine route snapshot emission to the controller
- make `PipeWireBackend` publish the runtime stream node ID
- make `AlsaExclusiveBackend` publish a stable static route anchor

Expected result:

- controller can subscribe to the active manager based on the active playback route

### Phase 4: Rebuild Final Graph In The Controller

Changes:

- move graph merge logic out of the engine
- move quality-analysis logic out of the engine
- make controller write `graph`, `quality`, and `qualityTooltip` into its returned playback snapshot

Expected result:

- UI-visible topology is produced entirely by the controller
- engine still runs playback, but not graph analysis

### Phase 5: Remove Backend Graph Callback Plumbing

Changes:

- remove `AudioRenderCallbacks::onGraphChanged`
- remove engine `handleGraphChanged()` and `analyzeAudioQuality()`
- remove backend code that pushes graph snapshots directly to the engine
- remove private `PipeWireMonitor` ownership from `PipeWireBackend`

Expected result:

- `IAudioBackend` becomes transport-only
- `PipeWireBackend` becomes materially simpler

### Phase 6: Cleanup And Consolidation

Changes:

- delete dead bridging code in the engine
- move any analysis-only helpers beside controller analysis code
- tighten documentation and tests around the new ownership model

## Recommended Commit Boundaries

Suggested implementation slices:

1. controller graph-state scaffolding and active-manager tracking
2. manager subscription API and static ALSA/null implementations
3. PipeWire manager subscription backed by the existing registry monitor
4. backend route-anchor reporting from PipeWire and ALSA
5. controller graph merge and controller quality analysis
6. removal of `onGraphChanged` and backend-side graph plumbing

These boundaries keep each change reviewable and make regressions easier to isolate.

## Required Tests

### Unit-Level Tests

Add focused tests for controller-owned analysis logic.

Recommended cases:

- decoder -> engine -> stream -> sink with identical formats yields `BitwisePerfect`
- lossy decoder source yields `LossySource`
- sample-rate mismatch yields `LinearIntervention`
- channel-count mismatch yields `LinearIntervention`
- integer widening yields `LosslessPadded`
- integer-to-float lossless mapping yields `LosslessFloat`
- multiple active sources feeding a reachable node yields `LinearIntervention`
- non-unity sink volume yields `LinearIntervention`
- muted sink yields `LinearIntervention`

Preferred location:

- `test/unit/app/` near other controller/logic tests

### Integration-Level Tests

Recommended integration checks:

- playback still starts and reports a stable route snapshot
- switching outputs tears down the old subscription and adopts the new one
- stopping playback clears graph state
- PipeWire graph updates do not crash when devices appear/disappear during playback

### Regression Coverage

At least one high-value regression test should verify that the tooltip and quality level remain stable for a known merged graph, because that is the user-visible outcome of this refactor.

## Validation Strategy

After each major phase:

1. build the debug configuration with the existing incremental build tree
2. run the affected playback/unit tests
3. manually verify the output selector still populates devices
4. manually verify the status bar still shows the playback path and quality icon
5. manually verify playback stop/switch/track-end does not leave stale graph state

Because this refactor touches playback core logic, tests should be run after the controller takes ownership of graph and quality fields.

## Acceptance Criteria

The refactor is complete when all of the following are true:

- `PlaybackEngine` no longer owns merged graph assembly
- `PlaybackEngine` no longer owns audio-quality analysis
- `PlaybackController` owns the final graph and quality result exposed to the UI
- `IBackendManager` supports graph subscription for the active playback route
- `IAudioBackend` no longer exposes `onGraphChanged`
- `PipeWireBackend` no longer owns a private topology monitor
- device enumeration still works across PipeWire and ALSA
- the status UI still shows a correct path tooltip and quality state

## Risks And Mitigations

### Risk: Stale Graph Updates Crossing Track Boundaries

Mitigation:

- generation counters on controller-owned subscriptions
- explicit subscription teardown on stop/switch/reopen

### Risk: PipeWire Stream Node ID Not Available Early Enough

Mitigation:

- publish route anchor asynchronously from backend when it becomes valid
- let the controller show only the internal graph until the anchor arrives

### Risk: The Quality Algorithm Assumes A Single Linear Path

Mitigation:

- require each manager subscription to normalize the active playback route down to one explicit stream node
- keep the merged graph semantics compatible with the current total-order traversal before attempting deeper algorithm changes

### Risk: Controller Becomes A New God Object

Mitigation:

- keep the logic limited to orchestration, merge, and analysis only
- if the analysis block becomes too large after the move, extract a private controller-side helper in a later cleanup commit, not before the ownership move is proven necessary

### Risk: Recomputing Graph And Quality Too Often

Mitigation:

- cache inputs and recompute only on change
- avoid doing analysis work directly inside the UI polling path when inputs are unchanged

## Final Recommendation

Proceed with this refactor.

The proposed direction matches the actual structural problem in the current codebase:

- `PlaybackEngine` is too smart
- `PlaybackController` is too passive
- `IBackendManager` is underused as a long-lived system observer
- `IAudioBackend` is overburdened with topology reporting

The safest implementation strategy is not to remove the old graph callback first. Instead:

1. create the manager subscription path
2. publish route anchors from the data plane
3. move graph merge and quality analysis to the controller
4. delete the old backend graph callback only after the new path is producing equivalent UI-visible results

That sequencing gives the cleanest architecture with the lowest migration risk.
