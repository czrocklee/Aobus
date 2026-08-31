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

Aobus ships four interactive or automation frontends over a shared C++ core and
application runtime, including a native WinUI desktop frontend.

```text
GTK ---+-> ao_app_uimodel -> ao_app_runtime -> core libraries
TUI ---+
WinUI -+
CLI -------------------> ao_app_runtime -> core libraries

GTK and WinUI -> ao_desktop_launch -> utility / Boost.Process

core libraries: utility, async, lmdb, media, library, query, audio
```

An arrow in this diagram means “depends on.”

The core libraries provide storage, encoded-media reading, query, asynchronous, and audio primitives without depending on application services or frontends.
`ao_app_runtime` composes those primitives into frontend-neutral services.
`ao_app_uimodel` turns runtime state and commands into platform-neutral presentation state and interaction policy.
`ao_desktop_launch` supplies GTK and WinUI with pure library-root, startup, and
successor-protocol rules plus detached process creation; it is not an
interactive runtime or frontend-lifecycle owner.
GTK and TUI bind runtime and UIModel state to their native event loops and rendering systems.
WinUI binds the same runtime and UIModel authorities to C++/WinRT, XAML,
DispatcherQueue, WASAPI, SMTC, and native Windows picker services.
The CLI uses `ao_app_runtime` directly when an interactive presentation model is unnecessary.

## Responsibilities

### Core libraries

The libraries under `lib/` and `include/ao/` own reusable mechanisms and domain storage.
They include the LMDB adapter and music library stores, media-file reading, reusable container parsing, query evaluation, asynchronous runtime primitives, and the audio engine and backends.
The async library owns executors, coroutine runtime and cancellation mechanisms, plus the shared owner-affine signal and subscription primitives used by runtime, UIModel, and frontends.

Core libraries do not own application workspace state, frontend lifecycle, user notifications, or cross-service orchestration.

### Application runtime

`ao_app_runtime` owns application-level state and coordinates core mechanisms.
Its public surface under `app/include/ao/rt/` includes the library facade, sources and projections, workspace and view services, playback services, completion, configuration, notifications, and frontend-neutral value types.
It also owns canonical cross-frontend paths derived from a supplied music-library root, without discovering platform application directories.
The derived cover cache follows that rule: each composition root resolves the application cache directory and passes it in, and the runtime consumes the path it is given.
Interactive-runtime-owned components may provide reusable application delivery behavior, such as coalesced and cached immutable resource-byte requests, without becoming `CoreRuntime` services.

`CoreRuntime` is the minimum composition used by non-interactive library clients such as the CLI.
It owns storage, asynchronous execution, the library facade and change bus, source caching, verified resource-byte reading, completion, and notifications.
`CoreRuntime::create()` is a typed-result factory: it opens and validates storage, allocates and finalizes its `Impl` and direct `MusicLibrary` first, uses the short-lived `Library::Prepared` token to acquire write authority against that final object, then emplaces the nonmovable `Library` directly in phase-local optional storage.
It completes the initial All Tracks source reload before exposing the runtime.
The `CoreRuntime` and `AppRuntime` wrappers remain move-only PImpl values; moving either public wrapper transfers only its unique PImpl and moved-from destruction is inert.

`AppRuntime` owns one `CoreRuntime` as the first direct member of its pinned implementation and adds the interactive application graph; it does not inherit from or expose the core owner.
It directly contains mandatory view, workspace, playback transport and succession, `PlaybackService`, and playback-session-persistence values; among those interactive additions, only the transferred nonmovable workspace store retains unique ownership.
The resolved playback-session store is a required reference, and one shared read-through `ResourceByteMemoryCache` is exposed through `resourceBytes()`.
Its public application face explicitly forwards the core library, async runtime, sources, notifications, completion, ordering policy, and music root, but not raw `MusicLibrary` or database-path access.
`AppRuntime::create()` is likewise the sole public construction boundary and returns a move-only value only after Core has moved into its final pinned implementation address and every Core-borrowing interactive member has been constructed there.
It exposes no partial graph when core initialization or required workspace-store composition fails.
It also owns narrow cross-service application commands, such as album reveal, that compose a workspace navigation result with a playback request without making either domain service depend on the other.
The [workspace architecture](workspace.md) owns the graph's view/workspace identities and semantic sessions.
The [interactive session lifecycle architecture](interactive-session-lifecycle.md) owns construction, restoration order, frontend-specific library transition, and teardown coordination.

### UIModel

`ao_app_uimodel` owns platform-neutral display projection, editing models, interaction policy, layout document models, and UI-local preference state.
It consumes runtime services and stable value types but does not become a second authority for storage, playback, or workspace state.

### Desktop application support

`ao_desktop_launch` owns application-level values and mechanisms shared only by
the two graphical desktop composition roots. Its public surface under
`app/include/ao/desktop/` contains library root identity, startup/switch plans,
the private successor protocol, and detached-launch policy. Sources under
`app/desktop/` perform filesystem inspection and Boost.Process creation.

It remains below GTK and WinUI and beside, rather than above, `AppRuntime` and
UIModel. It cannot inspect state stores, drive a toolkit event loop, checkpoint
a runtime, show failures, or claim graph teardown. TUI and CLI do not link it.

### Frontends

Each frontend is a composition root and platform adapter.
It selects the music root, explicit overrides, and platform application directories; constructs the appropriate executor and runtime; completes the runtime value's final frontend placement; transfers the fresh providers returned by core audio's platform factory; binds user events to commands; and owns toolkit or terminal lifecycle.
No provider registration, restoration, subscription, or frontend borrower may target a runtime wrapper that still has a move pending.
Composition roots translate recoverable runtime-factory errors into their existing startup or replacement presentation; no public throwing runtime constructor is retained as an adapter.
Frontends use the runtime path contract for standard per-library locations while retaining frontend-specific filenames and override policy.

GTK additionally owns widgets, CSS, dialogs, portals, GLib integration, and GTK-specific layout construction.
TUI owns FTXUI rendering, terminal input, overlays, and its event-loop adapter.
WinUI owns Windows App SDK application/window lifetime, XAML resources,
Windows dispatcher adaptation, native picker and media-session integration,
and its Modern and Classic presentation shells.
Its Windows-only target pair is `aobus-winui-lib`, which owns all compiled
frontend implementation, and the thin `aobus-winui` executable, which owns the
final link and deployed resources. The library includes the shell's own layout
catalog and dialect, element lattice, style resolution, themed surfaces,
responsive policy, desktop and theme schemas, XAML, and native adapters.
Windows-owned rules that need a native host are tested only by the native
Windows suite. Windows shell policy that carries no WinRT dependency is compiled
into `ao_core_test` on every host instead, so a Linux-only change cannot break it
unnoticed. Cross-desktop rules in `ao_desktop_launch` compile and run on both Linux
and Windows without creating a second WinUI model target on Linux.
The CLI owns argument parsing and output encoding around `CoreRuntime` operations.

## Boundaries and dependency direction

Dependencies follow the arrows toward core libraries and never reverse from runtime into UIModel or a frontend.

- Core libraries cannot include application or frontend headers.
- Runtime cannot depend on UIModel or platform UI types.
- Desktop support cannot depend on runtime, UIModel, storage/library internals,
  or platform UI types.
- UIModel may depend on runtime, but cannot include platform UI or direct storage/audio-control headers, and cannot name or speak for any one frontend even in portable code.
- Frontends may depend on runtime and UIModel and may contain platform adapters for core facilities.
- CLI behavior-bearing mutations use runtime facades where those roles exist; low-level inspection, dump, verification, relink, and interchange commands still use the `MusicLibrary` escape hatch exposed by `CoreRuntime`.
- Shared signal mechanisms live in `ao_async`, while the runtime or UIModel service that owns an event remains responsible for its payload, execution domain, and exception-containment policy.

Public runtime headers deliberately hide direct LMDB stores, library store/view types, and audio control-plane implementation types.
The check-owned `aobus_guardrails` target runs one declarative application
architecture audit across core public/source trees, desktop support, runtime,
UIModel, every frontend, and focused tests, so these edges are executable
constraints rather than diagram-only guidance.

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
| Media ingestion and identity | Encoded path -> `ao_media` file reader -> visitor-to-library runtime adapter and payload evidence -> stored records and resource descriptors | [Encoded media](encoded-media.md), [library](library.md), and [failure and reporting](failure-and-reporting.md) |
| Cover-art delivery | Stored descriptor -> runtime id -> derived cache or carrier media file -> owned bytes -> projection/playback state -> GTK, WinUI, TUI, MPRIS, or CLI transform | [Resource delivery](resource-delivery.md), [library](library.md), [playback](playback.md), and [presentation](presentation.md) |
| Track discovery and organization | UI authoring or CLI expression -> query compilation/evaluation -> live source membership -> projection shape -> frontend adaptation | [Track expression](track-expression.md), [library](library.md), and [presentation](presentation.md) |
| Interactive playback | Frontend command -> UIModel/runtime command -> workspace or live-source context -> succession and transport -> Player/Engine -> platform output | [Workspace](workspace.md), [playback](playback.md), and [runtime execution](runtime-execution.md) |
| Session restore and library transition | Frontend composition root -> managed state -> library-bound runtime graph -> workspace and playback restoration -> observers | [Persistence and managed state](persistence-and-managed-state.md), [interactive session lifecycle](interactive-session-lifecycle.md), [workspace](workspace.md), and [playback](playback.md) |
| Desktop shell construction | Shared layout language -> GTK-owned policy and widget tree, or WinUI-owned policy and native Modern/Classic XAML surfaces | [Application shell](application-shell.md), [presentation](presentation.md), and [persistence and managed state](persistence-and-managed-state.md) |
| Failure reporting | Subsystem failure -> typed result or event -> owning recovery boundary -> runtime notification or application-leaf presentation | [Failure and reporting](failure-and-reporting.md) plus the originating subsystem architecture |
| Audio-quality presentation | Engine and provider evidence -> Player analysis -> runtime snapshot -> shared UIModel interpretation -> GTK, TUI, or WinUI rendering | [Audio quality](audio-quality.md), refining [playback](playback.md) and [presentation](presentation.md) |

The [architecture landscape](README.md) owns the portfolio classification, relationship map, and capability coverage that connect these flows.

## Structural constraints

- One frontend runtime represents one active music library and owns every service tied to that library; [interactive session lifecycle](interactive-session-lifecycle.md) owns desktop successor-process restart, final placement, and the TUI's single-runtime lifetime, while [workspace](workspace.md) owns state within the graph.
- Move-only composition-root factories expose complete values, not ownership boxes; mandatory graph members are direct values or references.
  Phase-only presence is optional, and PImpl allocation pins implementation addresses across the wrapper's sole post-factory move.
- Cross-frontend domain behavior belongs in runtime or UIModel. Pure
  desktop-process selection and launch rules belong in `ao_desktop_launch`, not in
  parallel GTK and WinUI implementations.
- Runtime services expose stable application values and narrow command surfaces instead of leaking storage transactions or audio engine objects.
- UIModel state can be discarded and reconstructed from runtime state plus UI-local persisted preferences.
- Platform-specific names, widget types, CSS classes, terminal geometry, and event-loop handles stop at the frontend boundary.
- Exact schemas and command inventories are linked from reference documents rather than embedded in architecture.

## Failure, cancellation, and lifetime boundaries

Composition roots own runtime lifetime and destroy frontend observers before the runtime services they observe.
They first place each returned runtime value in its final stack, optional, or deliberately heap-pinned frontend owner, then publish runtime borrows.
`CoreRuntime` stops and joins worker execution before destroying library-backed collaborators.
`AppRuntime` destroys or quiesces its direct interactive borrowers in producer-first order, shuts down playback-session work and audio callback producers, and then shuts down and releases its first-member Core graph.

Recoverable failures cross core and runtime boundaries as typed results or typed runtime events.
The [failure and reporting architecture](failure-and-reporting.md) owns cross-layer classification, recovery, reporting, and application-leaf responsibilities.
The [outcome channel specification](../spec/failure/outcome-channel.md) owns shared channel and conversion behavior, and the [error value reference](../reference/failure/error.md) owns the exact common code surface.
Subsystem-specific code families and translations belong to their focused specifications and references; decoder behavior is owned by the [decoder session specification](../spec/playback/decoder-session.md) and [decoder error reference](../reference/playback/decoder-error.md).

## Implementation map

- [`lib/CMakeLists.txt`](../../lib/CMakeLists.txt) defines the core module graph and the `ao` umbrella target.
- [`Signal`](../../include/ao/async/Signal.h) and [`Subscription`](../../include/ao/async/Subscription.h) define the shared callback-delivery and connection-lifetime mechanisms below application layers.
- [`include/ao/media/file/`](../../include/ao/media/file/) and [`lib/media/file/`](../../lib/media/file/) form the library-neutral media-file sub-boundary within `ao_media`.
- [`app/CMakeLists.txt`](../../app/CMakeLists.txt) composes application targets; [`i18n`](../../app/i18n/CMakeLists.txt), [`runtime`](../../app/runtime/CMakeLists.txt), [`uimodel`](../../app/uimodel/CMakeLists.txt), and [`desktop`](../../app/desktop/CMakeLists.txt) own their source lists.
- [`app/include/ao/desktop/`](../../app/include/ao/desktop/) and
  [`app/desktop/`](../../app/desktop/) define the cross-desktop application
  support boundary.
- [`CoreRuntime`](../../app/include/ao/rt/CoreRuntime.h) is the non-interactive application composition.
- [`AppRuntime`](../../app/include/ao/rt/AppRuntime.h) is the interactive application composition.
- [`LibraryPaths`](../../app/include/ao/rt/library/LibraryPaths.h) derives the canonical per-library managed-data, database, and log locations from a selected music root.
- [`ResourceByteMemoryCache`](../../app/include/ao/rt/resource/ResourceByteMemoryCache.h) and [`ResourceBytes`](../../app/include/ao/rt/resource/ResourceBytes.h) own frontend-neutral read-through caching and independently owned encoded bytes shared by GTK, TUI, WinUI, and MPRIS consumers.
- [`app/linux-gtk/main.cpp`](../../app/linux-gtk/main.cpp), [`app/tui/App.cpp`](../../app/tui/App.cpp), [`app/windows-winui/App.xaml.cpp`](../../app/windows-winui/App.xaml.cpp), and [`CliRuntime`](../../app/cli/CliRuntime.cpp) are the frontend composition roots or bootstrap roots.
- [`ArchitectureAudit.cmake`](../../app/cmake/ArchitectureAudit.cmake) owns the declarative application-layer scan and composes the specialized UIModel, GTK, and WinUI structural checks.

## Test map

- [`AppRuntimeTest.cpp`](../../test/unit/runtime/AppRuntimeTest.cpp) protects value-factory failure isolation, move-only wrapper semantics, stable service identity across wrapper movement, interactive runtime composition, and service wiring.
- [`LibraryPathsTest.cpp`](../../test/unit/runtime/library/LibraryPathsTest.cpp) protects canonical per-library path derivation and existing-database detection.
- [`ResourceByteMemoryCacheTest.cpp`](../../test/unit/runtime/resource/ResourceByteMemoryCacheTest.cpp) protects bounded retention, coalescing, retry, callback affinity, cancellation, and destruction fencing.
- [`AsyncRuntimeTest.cpp`](../../test/unit/runtime/AsyncRuntimeTest.cpp) protects the shared execution mechanism.
- [`SignalTest.cpp`](../../test/unit/async/SignalTest.cpp) protects shared signal ordering, reentrancy, exceptions, and deferred lifetime.
- [`MainWindowTest.cpp`](../../test/unit/linux-gtk/app/MainWindowTest.cpp) and [`TuiRenderTestSupport.h`](../../test/unit/tui/TuiRenderTestSupport.h) support frontend-boundary tests.
- Tests under [`test/unit/desktop/`](../../test/unit/desktop/) protect the shared
  desktop boundary on Linux and Windows.
- [`CliSmokeTest.cpp`](../../test/unit/cli/CliSmokeTest.cpp) protects CLI use of the shared runtime.
- The `aobus_guardrails` target discovers the application architecture audit
  from [`ArchitectureAudit.cmake`](../../app/cmake/ArchitectureAudit.cmake), and the normal completion
  `./ao check` gate builds it explicitly.

## Related documents

- [Runtime execution architecture](runtime-execution.md)
- [Signal delivery specification](../spec/async/signal.md)
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
