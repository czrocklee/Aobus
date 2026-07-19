---
id: architecture.audio-quality
type: architecture
status: current
domain: playback
summary: Defines ownership and dependency flow for audio quality evidence from Core route providers through runtime, UIModel, GTK, and TUI.
---
# Audio quality architecture

## Scope

This document owns the cross-boundary structure of playback quality analysis.
It assigns ownership for route evidence, graph composition, analysis, runtime publication, platform-neutral presentation, and frontend adaptation from Core audio through GTK and TUI.

It does not define which conversion is lossless, how round-trip proof works, headline precedence, exact enum members, labels, or colors.
Those facts belong to the [audio quality specification](../spec/playback/quality-analysis.md) and [surface reference](../reference/playback/quality-surface.md).

## System context

Audio quality is a focused cross-cutting path through the layers established by the [system architecture](system-overview.md), within the broader ownership graph of the [playback architecture](playback.md):

```text
Core audio execution and providers
  Engine RouteStatus          BackendProvider graph
          \                    /
           v                  v
             Player merged graph
                     |
                     v
              QualityAnalyzer
                     |
                     v
Application runtime: PlaybackService QualityState + event
                     |
                     v
UIModel: shared verdict, labels, category, Soul aura
                 /           \
                v             v
              GTK             TUI
```

The relevant boundaries are:

| Boundary | System layer | Public surface | Implementation |
|---|---|---|---|
| Graph evidence and analysis | Core libraries | `include/ao/audio/` | `lib/audio/` and `lib/audio/backend/` |
| Accepted application snapshot and event | Application runtime | `app/include/ao/rt/playback/PlaybackService.h`, `PlaybackSnapshot.h` | `app/runtime/playback/PlaybackService.cpp`, `PlaybackTransport.cpp` |
| Shared presentation policy | UIModel | `app/include/ao/uimodel/playback/quality/` and `soul/` | `app/uimodel/playback/quality/` and `soul/` |
| Toolkit/terminal adaptation | Frontends | Frontend-local | `app/linux-gtk/playback/`, `app/linux-gtk/css/`, and `app/tui/` |

## Responsibilities

### Engine route evidence

Engine owns track-side route facts: encoded source format and lossiness, decoder output format, Engine output format, accepted route anchor, and playback generation.
It does not interpret these values into user-facing quality conclusions.

### Backend-provider evidence

The active `BackendProvider` owns platform route observation below Engine.
It publishes Stream, intermediary, Sink, external-source, volume-provenance, mute, format, and mixing evidence as a `flow::Graph` associated with an accepted route anchor.

Concrete providers decide what they can prove from ALSA, PipeWire, WASAPI, or another platform.
They report unknown or missing evidence honestly instead of manufacturing a complete path.

### Player composition and analysis

Player is the sole composition point for quality evidence.
It builds the Aobus source/decoder/Engine graph prefix, subscribes to the selected provider graph, connects the accepted Core and system paths, runs the pure analyzer, and stores the resulting graph and quality snapshot.

Player owns route-generation rejection, readiness, and callback marshalling.
`QualityAnalyzer` owns classification but does not subscribe to Engine, providers, runtime, or frontends.

### Runtime publication

`PlaybackService` adapts Player status into the frontend-neutral `QualityState` inside `PlaybackState`.
It refreshes the runtime snapshot before emitting `QualityChanged`, includes current readiness in that event, and exposes no provider subscription or graph-control object.

Runtime does not re-run analysis, assign finding severity, or format labels.

### UIModel presentation

UIModel owns shared interpretation for display: node/format/finding labels, one delivery headline, visual category, and Soul aura selection.
It consumes runtime quality values and does not traverse the provider graph or call Player/Engine.

The quality formatter owns presentation precedence independently of the analyzer's compatibility-oriented `overall` field.
The Soul view model additionally combines transport playing state and output readiness with quality state.

### Frontend adapters

GTK owns pipeline widgets, CSS classes, and volume-widget rendering.
TUI owns terminal pipeline layout and color adaptation.
Both consume UIModel values; neither owns a second severity table or headline policy.

## Boundaries and dependency direction

- Core audio quality types cannot depend on runtime, UIModel, GTK, or TUI.
- Backend graph producers depend only on Core audio flow values and platform facilities.
- Player may compose Engine and provider evidence but cannot emit frontend strings or categories.
- Runtime may expose stable Core quality values but cannot depend on UIModel formatting or frontend style types.
- UIModel may depend on runtime snapshots and Core value types but cannot include Player, Engine, Backend, or platform graph-control headers.
- GTK and TUI may adapt UIModel categories and labels but cannot reinterpret finding kinds into different severities.
- Platform volume-assistance state and quality evidence may originate from the same backend, but runtime volume presentation and analyzer findings remain separate consumers.

## Data and control flow

A track or route establishes the Core side first:

```text
decoder/Engine route state
  -> Player source + decoder + Engine nodes
  -> accepted route anchor and generation
  -> subscribe to active BackendProvider graph
```

Provider evidence then completes or refines the path:

```text
provider graph callback
  -> Player callback gate
  -> runtime callback executor
  -> route/playback generation check
  -> merge Core and provider graph
  -> analyzeAudioQuality(merged graph)
  -> Player status + quality callback
  -> PlaybackService refreshes PlaybackState
  -> QualityChanged(QualityState, ready)
  -> UIModel presentation
  -> GTK/TUI rendering
```

Provider graphs may change when a route appears, formats settle, external streams join or leave, mute/volume changes, or a backend changes volume provenance.
Each accepted graph replaces the previous provider evidence for that route and triggers a new derived result.

## Structural constraints

- There is one merged quality graph and one analyzer result per Player, not one interpretation per frontend.
- Engine owns Aobus-side route truth; the provider owns system-side observation; Player owns the join.
- The analyzer consumes immutable graph snapshots and owns no subscription or asynchronous lifetime.
- Player status, PlaybackService state, and `QualityChanged` preserve the same accepted analyzer result.
- Platform graph callbacks never mutate PlaybackService or UIModel directly.
- Missing provider evidence remains visible through verification state instead of being backfilled from desired route configuration.
- Frontends render ordered node assessments supplied by runtime and never reconstruct path order.
- Exact CSS classes and TUI colors are adapters for a shared UIModel category, not classification authorities.

## Failure, cancellation, and lifetime boundaries

Provider discovery, route activation, and graph observation can fail or remain incomplete without turning the pure analyzer into a recovery owner.
Player represents absent evidence as an empty/incomplete graph and readiness state; the behavioral consequences belong to the [quality specification](../spec/playback/quality-analysis.md).

Every provider callback is gated by Player lifetime and the route/playback generation that created its subscription.
Callbacks from a superseded route are ignored before graph replacement.
Foreign-thread graph callbacks marshal to the runtime callback executor before Player status or PlaybackService state changes.

During shutdown, the internal playback bootstrap closes transport publication before Player releases provider graph subscriptions and Engine activity.
The Player callback gate makes queued work a no-op after shutdown begins, so no quality update addresses destroyed runtime or UIModel state.
Frontend subscriptions are released before their runtime owner.

## Implementation map

- [`Engine.h`](../../include/ao/audio/Engine.h) and [`Engine.cpp`](../../lib/audio/Engine.cpp) own Aobus-side route status and generation.
- [`BackendProvider.h`](../../include/ao/audio/BackendProvider.h) and concrete code under [`lib/audio/backend/`](../../lib/audio/backend/) own platform graph publication.
- [`Graph.h`](../../include/ao/audio/flow/Graph.h) is the cross-provider evidence value.
- [`Player.cpp`](../../lib/audio/Player.cpp) owns graph merge, provider subscription, generation acceptance, analysis invocation, and callback marshalling.
- [`QualityAnalyzer.cpp`](../../lib/audio/QualityAnalyzer.cpp) is the pure classification boundary.
- [`PlaybackTransport.cpp`](../../app/runtime/playback/PlaybackTransport.cpp) adapts Player status into internal runtime state; [`PlaybackService.cpp`](../../app/runtime/playback/PlaybackService.cpp) carries accepted output, readiness, and quality in the coherent public snapshot.
- [`AudioQualityFormatter.cpp`](../../app/uimodel/playback/quality/AudioQualityFormatter.cpp) and [`AobusSoulViewModel.cpp`](../../app/uimodel/playback/soul/AobusSoulViewModel.cpp) own shared presentation.
- [`AudioPipelinePanel.cpp`](../../app/linux-gtk/playback/AudioPipelinePanel.cpp), [`AudioQualityCss.cpp`](../../app/linux-gtk/playback/AudioQualityCss.cpp), and [`QualityPanel.cpp`](../../app/tui/QualityPanel.cpp) are frontend adapters.

## Test map

- [`PlayerTest.cpp`](../../test/unit/audio/PlayerTest.cpp) protects graph composition, route-generation rejection, provider callback marshalling, and incomplete evidence.
- Backend tests under [`test/unit/audio/backend/`](../../test/unit/audio/backend/) protect provider graph ownership and publication.
- [`PlaybackTransportOutputTest.cpp`](../../test/unit/runtime/PlaybackTransportOutputTest.cpp) protects lower runtime snapshot/event adaptation, while [`PlaybackServiceTest.cpp`](../../test/unit/runtime/PlaybackServiceTest.cpp) protects coherent public correlation.
- [`AudioQualityFormatterTest.cpp`](../../test/unit/uimodel/playback/quality/AudioQualityFormatterTest.cpp) and [`AobusSoulViewModelTest.cpp`](../../test/unit/uimodel/playback/soul/AobusSoulViewModelTest.cpp) protect the UIModel boundary.
- [`AudioPipelinePanelTest.cpp`](../../test/unit/linux-gtk/playback/AudioPipelinePanelTest.cpp) and [`QualityIndicatorStyleTest.cpp`](../../test/unit/tui/QualityIndicatorStyleTest.cpp) protect frontend consumption of shared UIModel values.

## Related documents

- [System architecture](system-overview.md)
- [Playback architecture](playback.md)
- [Presentation architecture](presentation.md)
- [Runtime execution architecture](runtime-execution.md)
- [Audio quality analysis specification](../spec/playback/quality-analysis.md)
- [Audio quality surface reference](../reference/playback/quality-surface.md)
