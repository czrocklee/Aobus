---
id: architecture.system
type: architecture
status: current
domain: system
summary: Defines the top-level Aobus layers, dependency direction, composition roots, and subsystem boundaries.
---
# System architecture

## Scope

This document is the entry point for understanding how Aobus is divided and how its parts compose.
It owns the top-level layer model, dependency direction, composition-root responsibilities, and the placement of major subsystems.

It does not define user-visible behavior, serialized formats, command syntax, or detailed concurrency contracts.
Those facts belong in specifications, reference documents, and the focused architecture documents linked below.

## System context

Aobus ships three frontends over a shared C++ core and application runtime.

```text
GTK -> ao_app_uimodel -> ao_app_runtime -> core libraries
TUI -> ao_app_uimodel -> ao_app_runtime -> core libraries
CLI -----------------> ao_app_runtime -> core libraries

core libraries: utility, async, lmdb, media, library, query, audio
```

An arrow in this diagram means “depends on.”

The core libraries provide storage, encoded-media reading, query, asynchronous, and audio primitives without depending on application services or frontends.
`ao_app_runtime` composes those primitives into frontend-neutral services.
`ao_app_uimodel` turns runtime state and commands into platform-neutral presentation state and interaction policy.
GTK and TUI bind runtime and UIModel state to their native event loops and rendering systems.
The CLI uses `ao_app_runtime` directly when an interactive presentation model is unnecessary.

## Responsibilities

### Core libraries

The libraries under `lib/` and `include/ao/` own reusable mechanisms and domain storage.
They include the LMDB adapter and music library stores, media-file reading, reusable container parsing, query evaluation, asynchronous runtime primitives, and the audio engine and backends.

Core libraries do not own application workspace state, frontend lifecycle, user notifications, or cross-service orchestration.

### Application runtime

`ao_app_runtime` owns application-level state and coordinates core mechanisms.
Its public surface under `app/include/ao/rt/` includes the library facade, sources and projections, workspace and view services, playback services, completion, configuration, notifications, and frontend-neutral value types.

`CoreRuntime` is the minimum composition used by non-interactive library clients such as the CLI.
It owns storage, asynchronous execution, the library facade and change bus, source caching, completion, and notifications.

`AppRuntime` extends that composition for interactive applications.
It adds view and workspace services, playback transport and succession, audio-player ownership, and playback-session persistence.
The [workspace architecture](workspace.md) owns the graph's view/workspace identities and semantic sessions.
The [interactive session lifecycle architecture](interactive-session-lifecycle.md) owns construction, restoration order, active-library replacement, and teardown coordination.

### UIModel

`ao_app_uimodel` owns platform-neutral display projection, editing models, interaction policy, layout document models, and UI-local preference state.
It consumes runtime services and stable value types but does not become a second authority for storage, playback, or workspace state.

### Frontends

Each frontend is a composition root and platform adapter.
It selects paths, constructs the appropriate executor and runtime, registers platform audio providers, binds user events to commands, and owns toolkit or terminal lifecycle.

GTK additionally owns widgets, CSS, dialogs, portals, GLib integration, and GTK-specific layout construction.
TUI owns FTXUI rendering, terminal input, overlays, and its event-loop adapter.
The CLI owns argument parsing and output encoding around `CoreRuntime` operations.

## Boundaries and dependency direction

Dependencies follow the arrows toward core libraries and never reverse from runtime into UIModel or a frontend.

- Core libraries cannot include application or frontend headers.
- Runtime cannot depend on UIModel or platform UI types.
- UIModel may depend on runtime, but cannot include platform UI or direct storage/audio-control headers.
- Frontends may depend on runtime and UIModel and may contain platform adapters for core facilities.
- CLI behavior-bearing mutations use runtime facades where those roles exist; low-level inspection, dump, verification, relink, and interchange commands still use the `MusicLibrary` escape hatch exposed by `CoreRuntime`.

Public runtime headers deliberately hide direct LMDB stores, library store/view types, and audio control-plane implementation types.
The build attaches include-boundary checks to the runtime, UIModel, and GTK targets so these edges are executable constraints rather than diagram-only guidance.

## Data and control flow

A normal interactive command follows this direction:

```text
platform event
  -> frontend adapter
  -> UIModel policy or runtime command
  -> runtime service
  -> core storage/audio/query mechanism
```

State returns through snapshots and typed events:

```text
core result or callback
  -> runtime-owned state/change event
  -> UIModel projection when needed
  -> frontend rendering
```

Library mutations publish revisioned changes into runtime sources and projections instead of asking each frontend to reload storage independently.
Playback callbacks return through the runtime callback executor before runtime state and frontend observations are updated.

## Major system flows

These routes expose where a change crosses architecture owners without duplicating the detailed protocols those owners define.

| Flow | Top-level route | Focused architecture owners |
|---|---|---|
| Library maintenance | Frontend or CLI intent -> runtime library role -> core storage or external-data mechanism -> revisioned changes -> sources and projections | [Library](library.md), [runtime execution](runtime-execution.md), and [failure and reporting](failure-and-reporting.md) |
| Media ingestion and identity | Encoded path -> `ao_media` file reader -> visitor-to-library runtime adapter and payload evidence -> stored records and resources | [Encoded media](encoded-media.md), [library](library.md), and [failure and reporting](failure-and-reporting.md) |
| Cover-art delivery | Stored resource -> runtime id and owned bytes -> projection/playback state -> GTK, TUI, MPRIS, or CLI transform | [Resource delivery](resource-delivery.md), [library](library.md), [playback](playback.md), and [presentation](presentation.md) |
| Track discovery and organization | UI authoring or CLI expression -> query compilation/evaluation -> live source membership -> projection shape -> frontend adaptation | [Track expression](track-expression.md), [library](library.md), and [presentation](presentation.md) |
| Interactive playback | Frontend intent -> UIModel/runtime command -> workspace or live-source context -> succession and transport -> Player/Engine -> platform output | [Workspace](workspace.md), [playback](playback.md), and [runtime execution](runtime-execution.md) |
| Session restore and active-library replacement | Frontend composition root -> managed-state candidate -> library-bound runtime graph -> workspace and playback restoration -> observers | [Persistence and managed state](persistence-and-managed-state.md), [interactive session lifecycle](interactive-session-lifecycle.md), [workspace](workspace.md), and [playback](playback.md) |
| GTK shell construction | Layout preset and component state -> UIModel document/catalog policy -> GTK registries and factories -> widget tree and action bridge | [Application shell](application-shell.md), [presentation](presentation.md), and [persistence and managed state](persistence-and-managed-state.md) |
| Failure reporting | Subsystem failure -> typed result or event -> owning recovery boundary -> runtime notification or application-leaf presentation | [Failure and reporting](failure-and-reporting.md) plus the originating subsystem architecture |
| Audio-quality presentation | Engine and provider evidence -> Player analysis -> runtime snapshot -> shared UIModel interpretation -> GTK or TUI rendering | [Audio quality](audio-quality.md), refining [playback](playback.md) and [presentation](presentation.md) |

The [architecture landscape](README.md) owns the portfolio classification, relationship map, and capability coverage that connect these flows.

## Structural constraints

- One frontend runtime represents one active music library and owns every service tied to that library; [interactive session lifecycle](interactive-session-lifecycle.md) owns replacement of that graph, while [workspace](workspace.md) owns state within it.
- Cross-frontend behavioral policy belongs in runtime or UIModel, not in parallel GTK and TUI implementations.
- Runtime services expose stable application values and narrow command surfaces instead of leaking storage transactions or audio engine objects.
- UIModel state can be discarded and reconstructed from runtime state plus UI-local persisted preferences.
- Platform-specific names, widget types, CSS classes, terminal geometry, and event-loop handles stop at the frontend boundary.
- Exact schemas and command inventories are linked from reference documents rather than embedded in architecture.

## Failure, cancellation, and lifetime boundaries

Composition roots own runtime lifetime and destroy frontend observers before the runtime services they observe.
`CoreRuntime` stops and joins worker execution before destroying library-backed collaborators.
`AppRuntime` shuts down playback-session work and audio callback producers before its service graph is released.

Recoverable failures cross core and runtime boundaries as typed results or typed runtime events.
The [failure and reporting architecture](failure-and-reporting.md) owns cross-layer classification, recovery, reporting, and application-leaf responsibilities.
The [outcome channel specification](../spec/failure/outcome-channel.md) owns shared channel and conversion behavior, and the [error value reference](../reference/failure/error.md) owns the exact common code surface.
Subsystem-specific code families and translations belong to their focused specifications and references; decoder behavior is owned by the [decoder session specification](../spec/playback/decoder-session.md) and [decoder error reference](../reference/playback/decoder-error.md).

## Implementation map

- [`lib/CMakeLists.txt`](../../lib/CMakeLists.txt) defines the core module graph and the `ao` umbrella target.
- [`include/ao/media/file/`](../../include/ao/media/file/) and [`lib/media/file/`](../../lib/media/file/) form the library-neutral media-file sub-boundary within `ao_media`.
- [`app/CMakeLists.txt`](../../app/CMakeLists.txt) defines runtime, UIModel, frontend targets, and layer guardrails.
- [`CoreRuntime`](../../app/include/ao/rt/CoreRuntime.h) is the non-interactive application composition.
- [`AppRuntime`](../../app/include/ao/rt/AppRuntime.h) is the interactive application composition.
- [`app/linux-gtk/main.cpp`](../../app/linux-gtk/main.cpp), [`app/tui/App.cpp`](../../app/tui/App.cpp), and [`CliRuntime`](../../app/cli/CliRuntime.cpp) are the frontend composition roots.
- [`AssertNoForbiddenIncludes.cmake`](../../cmake/AssertNoForbiddenIncludes.cmake) and [`AssertUimodelOrganization.cmake`](../../cmake/AssertUimodelOrganization.cmake) enforce application-layer boundaries.

## Test map

- [`AppRuntimeTest.cpp`](../../test/unit/runtime/AppRuntimeTest.cpp) protects interactive runtime composition and service wiring.
- [`AsyncRuntimeTest.cpp`](../../test/unit/runtime/AsyncRuntimeTest.cpp) protects the shared execution mechanism.
- [`MainWindowTest.cpp`](../../test/unit/linux-gtk/app/MainWindowTest.cpp) and [`TuiRenderTestSupport.h`](../../test/unit/tui/TuiRenderTestSupport.h) support frontend-boundary tests.
- [`CliSmokeTest.cpp`](../../test/unit/cli/CliSmokeTest.cpp) protects CLI use of the shared runtime.
- Building `ao_app_runtime`, `ao_app_uimodel`, or `aobus-gtk-lib` runs the attached boundary guardrails from [`app/CMakeLists.txt`](../../app/CMakeLists.txt).

## Related documents

- [Runtime execution architecture](runtime-execution.md)
- [Failure and reporting architecture](failure-and-reporting.md)
- [Encoded media architecture](encoded-media.md)
- [Library architecture](library.md)
- [Resource delivery architecture](resource-delivery.md)
- [Track expression architecture](track-expression.md)
- [Playback architecture](playback.md)
- [Audio quality architecture](audio-quality.md)
- [Presentation architecture](presentation.md)
- [Application shell architecture](application-shell.md)
- [Workspace architecture](workspace.md)
- [Interactive session lifecycle architecture](interactive-session-lifecycle.md)
- [Persistence and managed-state architecture](persistence-and-managed-state.md)
- [Application-layer review](../development/application-layer-review.md) and [UIModel organization](../development/uimodel-organization.md)
