---
id: architecture.system
---
# System overview

Aobus has five interactive or automation frontends over shared Core libraries and an application runtime.
This page explains the layer boundaries, composition roots, and major routes through the system.
Detailed ordering, failure, storage, and lifecycle contracts live with their [topics](README.md).

## System context

An arrow means “depends on”:

```text
GTK ----+-> ao_app_uimodel -> ao_app_runtime -> core libraries
TUI ----+
WinUI --+
AppKit -+
CLI --------------------> ao_app_runtime -> core libraries

GTK, WinUI and AppKit -> ao_desktop_launch -> utility / Boost.Process
Linux GTK and TUI -----> ao_system_media_linux -> GIO / D-Bus

core libraries: utility, async, lmdb, media, library, query, audio
```

The AppKit frontend is an incremental native development slice.
Frontend differences and their native target/test boundaries are described in [frontend composition](frontend/README.md).

## Responsibilities

### Core libraries

`include/ao/` and `lib/` provide reusable mechanisms: storage, encoded-media reading, query evaluation, asynchronous execution, signals, and audio.
Core does not own interactive workspace state, user notifications, frontend lifecycle, or cross-service application orchestration.
See [library](library/README.md), [media](media/README.md), [expressions](query/README.md), and [playback](playback/README.md) for their models.

### Application runtime

`ao_app_runtime` composes Core into frontend-neutral services under `app/include/ao/rt/` and `app/runtime/`.
It owns library access, sources and projections, workspace/view state, playback, completion, notifications, and application commands.
It derives canonical per-library paths from a supplied music root; composition roots, not runtime, discover platform application and cache directories.

`CoreRuntime` is the minimum composition for non-interactive clients such as CLI.
`AppRuntime` composes a Core runtime with the interactive workspace and playback graph; it does not inherit from or publicly expose that Core owner.
Application commands may compose workspace and playback operations without making those services depend on each other.
Interactive consumers also share read-through resource-byte delivery rather than duplicating it in frontends.

Factories return complete runtime values or typed initialization failures, never a partially published graph.
The [session lifecycle](session-lifecycle.md#interactive-runtime-composition) owns final placement, pinned implementation, member construction order, restoration, and teardown.
The [library structure](library/structure.md) owns write-capability acquisition; [workspace](workspace/README.md) owns view identities and semantic sessions.

### UIModel

`ao_app_uimodel` converts runtime state into platform-neutral presentation and interaction policy.
It may own edit drafts, gestures, projections, formatting, layout values, and UI-local preference state, but not a second authority for library, playback, or workspace truth.
[Presentation](presentation/README.md) explains how it composes runtime values and frontend adapters.

### Desktop application support

`ao_desktop_launch`, under `app/include/ao/desktop/` and `app/desktop/`, supplies pure root selection, startup/switch plans, successor-protocol rules, and detached process creation to graphical frontends.
It is beside runtime and UIModel, not their lifecycle owner.
It cannot inspect state stores, drive event loops, checkpoint a runtime, show failures, or tear down the graph.
TUI and CLI do not link it; [desktop lifecycle](desktop-library-lifecycle.md) owns the shared handoff contract.

### Application platform adapters

`ao_system_media_linux`, under `app/platform/media/linux/`, provides shared Linux MPRIS integration to GTK and TUI.
It consumes runtime playback state, UIModel playback actions, and an executor supplied by the frontend composition root.
It does not enter Core, runtime, or UIModel, and it has no GTK or FTXUI dependency.
Each bridge owns an optional private GIO connection and native context while live runtime queries and host callbacks remain on the frontend's existing callback executor.
See [Linux MPRIS](frontend/mpris.md) for its transfer and teardown contract.

### Frontends

Each frontend is a composition root and platform adapter.
It selects the root and platform paths, supplies an executor, constructs the runtime, transfers fresh audio providers from Core's platform factory, binds events, and owns toolkit or terminal resources.
The runtime must reach its final placement before any provider registration, restoration, subscription, or borrower targets it.
Frontend observers retire before the services they observe; see [session lifecycle](session-lifecycle.md).

GTK owns GLib/GTK, CSS, dialogs, portals, native layout construction, and composition of the optional shared Linux MPRIS adapter.
TUI owns FTXUI input, rendering, geometry, overlays, its event-loop adapter, and composition of that same adapter on supported Linux builds without linking GTK.
WinUI owns Windows App SDK/XAML, DispatcherQueue, pickers, WASAPI integration, and SMTC.
AppKit owns native windows and controls, the main run loop, and Core Audio integration.
CLI owns arguments and output around `CoreRuntime`, bypassing UIModel when no interactive projection is needed.

## Boundaries and dependency direction

- Core cannot include application or frontend headers.
- Runtime cannot depend on UIModel or platform UI types. Public runtime headers hide direct LMDB stores, library store/view types, and audio control-plane implementation types.
- Desktop support cannot depend on runtime, UIModel, library/storage internals, or platform UI types.
- UIModel may depend on runtime interfaces and stable values, but cannot include platform UI or direct storage/audio-control headers, or speak for a named frontend even in portable code.
- Frontends and application platform adapters may consume runtime/UIModel and adapt Core platform facilities; platform names, handles, widget types, CSS classes, D-Bus names, and terminal geometry stop at those boundaries.
- CLI behavior-bearing mutations use runtime facades where available. Low-level inspection, dump, verification, relink, and interchange may use the `MusicLibrary` escape hatch exposed by `CoreRuntime`.
- Shared signals live in `ao_async`; each event owner still defines payload, executor affinity, and exception containment.

These are executable constraints, not only diagram conventions.
The check-owned `aobus_guardrails` target runs the declarative [application architecture audit](../../app/cmake/ArchitectureAudit.cmake) across Core, desktop support, runtime, UIModel, frontends, and focused tests.

## Data and control flow

```text
platform event -> frontend adapter -> UIModel policy or runtime command
               -> runtime service -> Core mechanism

Core result/callback -> runtime snapshot or typed event
                     -> UIModel projection when needed -> frontend rendering
```

Runtime owns cross-frontend truth; UIModel can be rebuilt from runtime state and its own persisted preferences.
Library mutations publish revisioned changes rather than asking every frontend to reload storage.
Playback observations return through the callback executor before application state is published.

## Major system flows

| Change | Follow these contracts |
|---|---|
| Maintain the library | [Mutation](library/mutation.md) → [publication](library/change-publication.md) → [sources](library/track-source.md) and [projections](library/track-list-projection.md) |
| Read files or reconcile identity | [Media](media/README.md) → [scan and identity](library/scan-and-identity.md) |
| Deliver artwork | [Resource identity](../reference/resource/blob.md) → [verified delivery and frontend transforms](resource/cover-art-delivery.md) |
| Discover and organize tracks | [Expressions](query/README.md) → [library sources](library/track-source.md) → [presentation](presentation/README.md) |
| Play music | [Workspace](workspace/README.md) → [playback commits](playback/application-commit.md) and [succession](playback/cursor.md) → [audio execution](playback/audio-execution.md) |
| Restore, switch library, or close | [Persistence](persistence/README.md) → [session lifecycle](session-lifecycle.md) → [frontend adaptation](frontend/README.md) |
| Build a shell | [Shared layout and actions](shell/README.md) → [frontend-owned construction and policy](frontend/README.md) |
| Propagate or report failure | Originating [subsystem](README.md) → [outcome channels](failure/outcome-channel.md) → [recovery/reporting owner](failure/README.md) |
| Explain audio fidelity | [Quality evidence](playback/quality.md) → [analysis](playback/quality-analysis.md) → [presentation](playback/quality-values.md) |

## Implementation and tests

- [`lib/CMakeLists.txt`](../../lib/CMakeLists.txt) and [`app/CMakeLists.txt`](../../app/CMakeLists.txt) define the target graph.
- [`CoreRuntime`](../../app/include/ao/rt/CoreRuntime.h), [`AppRuntime`](../../app/include/ao/rt/AppRuntime.h), and [`LibraryPaths`](../../app/include/ao/rt/library/LibraryPaths.h) expose runtime composition and per-library paths.
- [`AppRuntimeTest.cpp`](../../test/unit/runtime/AppRuntimeTest.cpp) protects factory failure isolation, stable identity, and graph composition; [`LibraryPathsTest.cpp`](../../test/unit/runtime/library/LibraryPathsTest.cpp) protects path derivation.
- [Execution](execution/README.md), [session lifecycle](session-lifecycle.md), and the subsystem pages locate focused implementation and contract tests.

For contribution procedures rather than product boundaries, use [application-layer review](../development/application-layer-review.md) and [UIModel organization](../development/uimodel-organization.md).
